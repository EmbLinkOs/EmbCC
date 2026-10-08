#include "target.h"

#include "../platform/platform.h"

#include <string.h>

#include "../elf/elf.h"
#include "../coff/coff.h"
#include "../macho/macho.h"

static enum target_arch g_arch = TARGET_X86_64;
static int g_thumb_em;      /* --target=thumbv7em-*: see target_thumb_em */
/* The Thumb architecture LEVEL: 7 for ARMv7-M and 8 for ARMv8-M Mainline
 * (Cortex-M33, which the RTOS requirements name as its third target), and
 * 6 for ARMv6-M (Cortex-M0, M0+, M1), whose instruction set is a SUBSET:
 * Thumb-1 and six 32-bit instructions, selected by codegen.c (v6m.c).
 *
 * A level and not a new enum target_arch value, because that enum keys the
 * DATA MODEL -- D-016's reasoning for RISC-V being two targets -- and
 * ARMv8-M's is identical to ARMv7-M's: ILP32, the same sizes, the same
 * AAPCS32. What differs is the instruction set's CEILING (v8-M Mainline is
 * a superset), the predefined macros, and two .ARM.attributes tags. A
 * second enum value would duplicate a data model to express none of that. */
static int g_thumb_arch = 7;
/* ARMv8-M Baseline (Cortex-M23): level 6 -- the ARMv6-M instruction
 * selection, v6m.c, and everything else level 6 means (no FPU, no IT, an
 * unaligned access faults) -- with what Baseline adds on top: the
 * divides, the exclusives, MOVW/MOVT, CBZ and B.W, and the security
 * extension. A flag beside the level rather than a level of its own,
 * because nearly every question asked of level 6 has the same answer
 * here; the few that differ ask target_thumb_v8m_base(). */
static int g_thumb_v8b;
/* -mcmse: the Secure-side build of ARMv8-M's security extension. */
static int g_thumb_cmse;
static int g_thumb_fpu;     /* see target_thumb_fpu */
static int g_thumb_fpu_dp;  /* see target_thumb_fpu_dp */
static int g_thumb_hard;    /* see target_thumb_hard_abi */
static int g_thumb_hf_name; /* the triple asked for was an -eabihf one */
/* ARMv7-A in ARM state (armv7a-none-eabi): the same AAPCS32 and data model
 * as the Cortex-M levels -- which is why it is this target and not a new
 * enum value -- with the A32 instruction set. See target_arm_a32. */
static int g_arm_a32;
/* ...and its floating-point unit, when -mfpu= names one: 3 or 4 for VFPv3
 * or VFPv4, `d32` for the 32-register file (the D16 units have 16). Only
 * read when target_thumb_fpu() says the code uses an FPU. */
static int g_arm_vfp = 3, g_arm_vfp_d32;
static enum target_os   g_os   = TGT_OS_NONE;
static enum target_fmt  g_fmt  = TGT_FMT_ELF;
/* The target's byte order: 1 for big-endian (mips-none-elf), 0 for every
 * other target. Set with the triple, and by -EB/-EL on MIPS. */
static int g_big_endian;

int target_big_endian(void) { return g_big_endian; }
void target_set_big_endian(int on) { g_big_endian = on ? 1 : 0; }

void target_put_uint(unsigned char *p, int n, unsigned long long v)
{
    for (int b = 0; b < n; b++)
        p[g_big_endian ? n - 1 - b : b] = (unsigned char)(v >> (8 * b));
}

unsigned long long target_get_uint(const unsigned char *p, int n)
{
    unsigned long long v = 0;
    for (int b = 0; b < n; b++)
        v |= (unsigned long long)p[g_big_endian ? n - 1 - b : b] << (8 * b);
    return v;
}

int target_byte_shift(int off, int size, int whole)
{
    return 8 * (g_big_endian ? whole - off - size : off);
}

enum target_arch target_get(void) { return g_arch; }
void target_set(enum target_arch a) { g_arch = a; }

static int g_opt_size;
void target_set_opt_size(int on) { g_opt_size = on; }
int  target_opt_size(void)       { return g_opt_size; }

const char *target_default_name(void)
{
    const char *t = plat_getenv("EMBCC_DEFAULT_TARGET");

#ifdef EMBCC_DEFAULT_TARGET
    /* The environment wins over the compiled-in one, so a person can
     * switch boards for a shell without rebuilding the compiler. */
    if (!t || !*t)
        t = EMBCC_DEFAULT_TARGET;
#endif
    return t && *t ? t : NULL;
}

int target_insn_len(const unsigned char *p, int avail)
{
    if (avail <= 0)
        return 0;
    switch (g_arch) {
    case TARGET_AARCH64:
        /* A64 is fixed 32-bit, with no exceptions. */
        return avail >= 4 ? 4 : 0;

    case TARGET_RISCV32:
    case TARGET_RISCV64:
        /* The RISC-V length encoding lives in the low bits of the first
         * halfword (unprivileged ISA, "Expanded Instruction-Length
         * Encoding"). EmbCC emits no compressed instructions today, but
         * the rule is written in full: adding the C extension must not
         * also require remembering to change this. */
        if ((p[0] & 0x03) != 0x03)
            return avail >= 2 ? 2 : 0;          /* 16-bit (C extension) */
        if ((p[0] & 0x1f) != 0x1f)
            return avail >= 4 ? 4 : 0;          /* the base 32-bit forms */
        return 0;                               /* 48-bit and wider: unused */

    case TARGET_THUMB: {
        /* ARM state (armv7a): one word, always -- grouped by the Thumb
         * rule below, -S put each relocation two bytes into the
         * instruction before its own. */
        if (g_arm_a32)
            return avail >= 4 ? 4 : 0;
        /* Thumb-2: a first halfword whose top five bits are 0b11101,
         * 0b11110 or 0b11111 introduces a 32-bit instruction; everything
         * else is one halfword (ARMv7-M ARM, A5.1). */
        if (avail < 2)
            return 0;
        unsigned hw = (unsigned)p[0] | ((unsigned)p[1] << 8);   /* little-endian */
        if ((hw & 0xf800u) >= 0xe800u)
            return avail >= 4 ? 4 : 0;
        return 2;
    }

    case TARGET_AVR: {
        /* One 16-bit word, but for the four that carry an address in a
         * second: JMP and CALL (1001 010k kkkk 11xk) and the 32-bit LDS
         * and STS (1001 00sd dddd 0000). Without this AVR's -S grouped
         * its bytes by x86 lengths and ran past function ends. */
        if (avail < 2)
            return 0;
        unsigned w = (unsigned)p[0] | ((unsigned)p[1] << 8);
        if ((w & 0xfe0cu) == 0x940cu || (w & 0xfc0fu) == 0x9000u)
            return avail >= 4 ? 4 : 0;
        return 2;
    }

    case TARGET_MIPS32:
    case TARGET_MIPS64:
        /* MIPS is fixed 32-bit; microMIPS and MIPS16e are not emitted. */
        return avail >= 4 ? 4 : 0;

    case TARGET_LOONGARCH64:
        /* LoongArch is fixed 32-bit, with no compressed forms at all. */
        return avail >= 4 ? 4 : 0;
    case TARGET_TRICORE:
        /* Bit 0 of the first byte: set for a 32-bit instruction, clear
         * for a 16-bit one (TriCore 1.6 architecture manual, vol. 2). */
        if (p[0] & 1)
            return avail >= 4 ? 4 : 0;
        return avail >= 2 ? 2 : 0;
    case TARGET_XTENSA: {
        /* Three bytes, or two for a density instruction: op0 (the low
         * nibble of the first byte) 8..13. EmbCC emits only the
         * three-byte forms, and a literal pool's words are data. */
        int n = (p[0] & 15) >= 8 && (p[0] & 15) <= 13 ? 2 : 3;
        return avail >= n ? n : 0;
    }
    case TARGET_PPC32:
        /* Book E PowerPC is fixed 32-bit; VLE is refused, not emitted. */
        return avail >= 4 ? 4 : 0;
    case TARGET_RX:
        /* One to eight bytes, and no rule short of decoding: as x86. */
        return 0;
    case TARGET_SPARC32:
        /* SPARC V8 is fixed 32-bit. */
        return avail >= 4 ? 4 : 0;
    case TARGET_COLDFIRE:
        /* One to five 16-bit words, decided by the operation word and its
         * effective addresses. -S groups the stream by word: its bytes are
         * written as data either way, and there is no m68k assembler here
         * to read the text back. */
        return avail >= 2 ? 2 : 0;

    case TARGET_X86_64:
    default:
        /* Variable-length, and no rule short of decoding it. The caller
         * asks the disassembler. */
        return 0;
    }
}

int target_apply_default(const char **bad)
{
    const char *t = target_default_name();
    enum target_arch a;
    enum target_os os;
    enum target_fmt fmt;

    if (!t)
        return 1;                    /* the built-in default stands */
    if (!target_from_triple(t, &a, &os, &fmt)) {
        *bad = t;
        return 0;
    }
    target_set(a);
    target_os_set(os);
    target_fmt_set(fmt);
    return 1;
}

enum target_os  target_os_get(void)  { return g_os; }
enum target_fmt target_fmt_get(void) { return g_fmt; }
void target_os_set(enum target_os o)   { g_os = o; }
void target_fmt_set(enum target_fmt f) { g_fmt = f; }

/* The data model. One table rather than a switch per question, so a
 * new architecture is one row and the compiler will not build until
 * every column of it is filled in. */
static const struct data_model {
    /* `dbl` is here because AVR's `double` is FOUR bytes -- avr-gcc's
     * documented default, and confirmed against clang --target=avr. Every
     * other target has eight, which is why ty_size() answered 8 outright
     * until now; a column makes the exception explicit instead of
     * hiding it in one architecture's backend. A four-byte double also
     * means `double` arithmetic uses the FLOAT helpers (__addsf3, not
     * __adddf3), which is what avr-gcc does. */
    /* `it` is int's width. Two on AVR, four everywhere else -- the
     * other half of what makes AVR not a 32-bit machine, and hardcoded
     * as 4 until now for the same reason double was hardcoded as 8. */
    /* `maxal` caps a SCALAR's alignment, 0 meaning no cap: every scalar is
     * aligned to its own size unless this says less. It says 1 on AVR,
     * where nothing needs alignment -- avr-gcc and clang give _Alignof of
     * every type as 1 -- and nothing anywhere else, so no other target's
     * layout moves. Without it, alignment was the size on every target, and
     * `struct { char c; int i; }` was four bytes on AVR where avr-gcc makes
     * it three: a struct shared with avr-gcc-built code, laid over a
     * register block or sent down a wire came out a different shape. */
    int ptr, lng, it, dbl, ldbl, char_uns, wchar_uns, int128, maxal;
} g_model[] = {
    /* x86-64 System V: LP64, signed char, x87 long double in 16 bytes */
    [TARGET_X86_64]  = { 8, 8, 4, 8, 16, 0, 0, 1, 0 },
    /* AAPCS64: LP64, UNSIGNED char and wchar_t, binary128 long double */
    [TARGET_AARCH64] = { 8, 8, 4, 8, 16, 1, 1, 1, 0 },
    /* AAPCS (32-bit, EABI): ILP32, unsigned char and wchar_t, and a
     * long double that is an ordinary IEEE double -- checked against
     * clang -target thumbv7m-none-eabi -dM, which gives
     * __SIZEOF_LONG_DOUBLE__ 8 and __LDBL_MANT_DIG__ 53. long long
     * stays 8, and is 8-ALIGNED, which is where a 32-bit ABI most
     * often surprises: __BIGGEST_ALIGNMENT__ is 8, not 4. */
    [TARGET_THUMB]   = { 4, 4, 4, 8, 8, 1, 1, 0, 0 },
    /* The RISC-V psABI. Unsigned char like the ARM ones, but a SIGNED
     * wchar_t -- which is why those are two columns and not one -- and
     * a binary128 long double at both widths. Read off
     * `clang -target riscv{32,64}-unknown-elf -dM`. */
    [TARGET_RISCV32] = { 4, 4, 4, 8, 16, 1, 0, 0, 0 },
    [TARGET_RISCV64] = { 8, 8, 4, 8, 16, 1, 0, 1, 0 },
    /* AVR (avr-gcc's ABI, measured against clang --target=avr
     * -mmcu=atmega328p): 16-bit pointers -- the first target here where
     * a pointer is NARROWER than a long -- a four-byte double, and no
     * __int128 on an 8-bit machine.
     *
     * `char` is SIGNED here, which is what clang --target=avr does and
     * what the generated predefined-macro table therefore says. It is
     * very likely NOT what avr-gcc does -- avr-gcc is documented as
     * defaulting to -funsigned-char -- but there is no avr-gcc on this
     * machine to check, and the alternative was to set the model from
     * recollection and hand-edit a GENERATED table to agree with it.
     *
     * So: the compiler is self-consistent, every part of it verifiable
     * against something real, and the open question is written down
     * rather than guessed. Settling it needs a real avr-gcc, and it
     * matters before the kernel is built: char's default signedness
     * changes what `char c = 200; c > 0` answers, though not the ABI.
     *
     * `long double` is four bytes too: the same type as double, which is
     * the same type as float. There is no wider floating point on this
     * machine. */
    [TARGET_AVR]     = { 2, 4, 2, 4,  4, 0, 0, 0, 1 },
    /* o32 (clang --target=mipsel-unknown-elf -dM): ILP32, a SIGNED char
     * -- unlike the ARM and RISC-V targets beside it -- a signed int
     * wchar_t, long double the same 8-byte double, and no __int128. */
    [TARGET_MIPS32]  = { 4, 4, 4, 8,  8, 0, 0, 0, 0 },
    /* LP64S (clang --target=loongarch64-unknown-elf -msoft-float -dM): LP64
     * like RV64, a binary128 long double and __int128 -- but a SIGNED
     * char, which RISC-V's is not, and a signed int wchar_t. */
    [TARGET_LOONGARCH64] = { 8, 8, 4, 8, 16, 0, 0, 1, 0 },
    /* The TriCore EABI as remembered (unverified; no TriCore compiler
     * here): ILP32, a SIGNED char, a signed int wchar_t, long double the
     * same 8-byte double, no __int128 -- and nothing aligned beyond a
     * word, since a TriCore doubleword access needs only that: long long
     * and double are 4-aligned (maxal 4). */
    [TARGET_TRICORE] = { 4, 4, 4, 8,  8, 0, 0, 0, 4 },
    /* Xtensa (xtensa-esp32-elf-gcc -dM): ILP32, UNSIGNED char, a 16-bit
     * UNSIGNED wchar_t (target_wchar_size), long double the 8-byte
     * double, and no __int128. */
    [TARGET_XTENSA]  = { 4, 4, 4, 8,  8, 1, 1, 0, 0 },
    /* The PowerPC EABI (clang --target=powerpc-none-eabi -mcpu=e500
     * -mno-spe -msoft-float -mlong-double-64 -dM): ILP32, an UNSIGNED char,
     * a signed int wchar_t, long double the 8-byte double, no __int128. */
    [TARGET_PPC32]   = { 4, 4, 4, 8,  8, 1, 0, 0, 0 },
    /* RX (rx-elf-gcc -nofpu -dM): ILP32 with an UNSIGNED char, a signed
     * `long int` wchar_t, double and long double both binary32 (GCC's
     * -m32bit-doubles default), no __int128, and nothing aligned beyond
     * 4 (__BIGGEST_ALIGNMENT__ 4: a long long is 4-aligned). */
    [TARGET_RX]      = { 4, 4, 4, 4,  4, 1, 0, 0, 4 },
    /* SPARC V8 (clang --target=sparc-none-elf -mcpu=leon3 -dM): ILP32, a
     * SIGNED char and wchar_t, a binary128 long double -- and nothing
     * aligned beyond 8 (__BIGGEST_ALIGNMENT__ 8: _Alignof(long double) is
     * 8), which is the cap column's second use after AVR's 1. */
    [TARGET_SPARC32] = { 4, 4, 4, 8, 16, 0, 0, 0, 8 },
    /* ColdFire as GCC's m68k-elf lays it out (docs/internals/coldfire-plan.md,
     * unverified -- there is no m68k compiler here): ILP32, a SIGNED char
     * and wchar_t, long double the 8-byte double, no __int128, and NOTHING
     * aligned beyond two bytes (BIGGEST_ALIGNMENT 16 bits without
     * -malign-int): an int in a struct may sit at offset 2. */
    [TARGET_COLDFIRE] = { 4, 4, 4, 8,  8, 0, 0, 0, 2 },
    /* n64 (clang --target=mips64el-none-elf -msoft-float -dM): LP64, a
     * SIGNED char as on o32, a signed int wchar_t, a binary128 long
     * double and __int128. */
    [TARGET_MIPS64]  = { 8, 8, 4, 8, 16, 0, 0, 1, 0 },
};

int target_is_mips(void)
{
    return g_arch == TARGET_MIPS32 || g_arch == TARGET_MIPS64;
}

int target_ptr_size(void)       { return g_model[g_arch].ptr; }
int target_double_size(void)    { return g_model[g_arch].dbl; }
int target_int_size(void)       { return g_model[g_arch].it; }
/* XLEN is RISC-V's own name for the register width IN BITS -- 32 or 64,
 * the number in `rv32`/`rv64` and in __riscv_xlen. Returning the pointer
 * column directly would give BYTES, and the backend that divided it by 8
 * to get bytes got 1 and made every value a byte wide. */
int target_xlen(void)           { return g_model[g_arch].ptr * 8; }
int target_long_size(void)      { return g_model[g_arch].lng; }
int target_max_scalar_align(void) { return g_model[g_arch].maxal; }
int target_default_new_align(void)
{
    return g_arch == TARGET_THUMB ? 8 : 16;
}
int target_has_sqrt(int bytes)
{
    switch (g_arch) {
    case TARGET_X86_64:
    case TARGET_AARCH64: return bytes == 4 || bytes == 8;
    /* FPv5-D16 has vsqrt.f64 as well; the single-precision units do not,
     * and a double's square root there is libm's. */
    case TARGET_THUMB:   return target_thumb_fpu() &&
                                (bytes == 4 ||
                                 (bytes == 8 && target_thumb_fpu_dp()));
    default:             return 0;
    }
}

int target_has_mulh(void)
{
    switch (g_arch) {
    /* A 64-bit register holds the whole product of two 32-bit values,
     * so these take the ordinary 64-bit multiply and never see one. */
    case TARGET_X86_64:
    case TARGET_AARCH64:
    case TARGET_RISCV64:
    case TARGET_LOONGARCH64:
    case TARGET_MIPS64:
        return 0;
    /* RV32IM: mulh, mulhu (the M extension is always present here) */
    case TARGET_RISCV32: return 1;
    /* umull and smull from ARMv7-M up, and in ARM state; ARMv6-M and
     * ARMv8-M Baseline have only the 32-bit muls */
    case TARGET_THUMB:   return target_thumb_arch() >= 7 &&
                                !target_thumb_v8m_base();
    /* mult/multu and HI */
    case TARGET_MIPS32:  return 1;
    case TARGET_TRICORE: return 1;    /* MUL and MUL.U into an E register */
    case TARGET_PPC32:   return 1;    /* mulhw, mulhwu */
    case TARGET_RX:      return 1;    /* emul, emulu */
    case TARGET_SPARC32: return 1;    /* umul and smul, and %y */
    /* Xtensa's high multiplies (mulsh, muluh) are the MUL32_HIGH option,
     * which the ESP32 has and the de212 core EmbCC is tested on does not
     * (docs/internals/xtensa-plan.md): its quou and remu stay. ColdFire
     * has only the 32-bit muls.l and mulu.l -- the 64-bit result forms
     * are the 68020's -- and AVR none at all. */
    case TARGET_XTENSA:
    case TARGET_COLDFIRE:
    case TARGET_AVR:
        return 0;
    }
    return 0;
}

int target_stack_align(void)
{
    switch (g_arch) {
    case TARGET_THUMB: return 8;        /* AAPCS32 at a public interface */
    case TARGET_MIPS32: return 8;       /* o32 */
    case TARGET_TRICORE: return 8;      /* the TriCore EABI */
    case TARGET_RX:     return 4;       /* STACK_BOUNDARY 32 */
    case TARGET_SPARC32: return 8;      /* the SPARC V8 ABI */
    case TARGET_COLDFIRE: return 4;     /* ColdFire's preferred boundary */
    case TARGET_AVR:   return 1;
    default:           return 16;       /* SysV, AAPCS64, RISC-V psABI */
    }
}
/* Apple's arm64 is not AAPCS64's data model in three columns, read off
 * `clang -target arm64-apple-macos -dM`: plain char is SIGNED, wchar_t
 * is `int`, and long double is double. EmbCC gave macOS the Linux model
 * -- so `(char)-1 < 0` was false, and a long double passed to libSystem
 * was sixteen bytes where it reads eight. */
static int darwin_a64(void)
{
    return g_arch == TARGET_AARCH64 && g_os == TGT_OS_DARWIN;
}

int target_ldouble_size(void)
{
    return darwin_a64() ? 8 : g_model[g_arch].ldbl;
}
static int g_char_uns_override = -1;

void target_set_char_signed(int unsigned_char)
{
    g_char_uns_override = unsigned_char;
}

int target_char_unsigned(void)
{
    return g_char_uns_override >= 0 ? g_char_uns_override
           : darwin_a64() ? 0 : g_model[g_arch].char_uns;
}
int target_wchar_size(void)
{
    return g_arch == TARGET_XTENSA ? 2 : g_model[g_arch].it;
}

int target_wchar_unsigned(void)
{
    return darwin_a64() ? 0 : g_model[g_arch].wchar_uns;
}
int target_has_int128(void)     { return g_model[g_arch].int128; }
/* -fno-jump-tables. AVR never has a table (its indirect jump wants a
 * word address in Z, and the compare tree is as small); everywhere else
 * a dense switch gets one unless this says not. irgen's switch_dense is
 * the only reader, and IR_SWITCH -- which every backend lowers to its
 * table -- comes from nowhere else, so this one answer is the whole of
 * the promise. */
static int g_no_jump_tables;
void target_set_jump_tables(int on) { g_no_jump_tables = !on; }
/* TriCore and Xtensa keep the decision tree for now: a table needs its
 * own address (TriCore: a HI/LO2 pair against .text or a JL; Xtensa: the
 * base from a literal and a jx), and neither lowering is written --
 * IR_SWITCH is refused by name if one ever arrives. */
int target_jump_tables(void)
{
    return !g_no_jump_tables && target_get() != TARGET_AVR &&
           target_get() != TARGET_TRICORE &&
           target_get() != TARGET_XTENSA &&
           target_get() != TARGET_RX;
}
/* -Os: a switch dense but for a few outlying cases gets a table over the
 * dense run (irgen's switch_cluster). ARM only so far: measured there,
 * where a byte table (tbb) is cheaper than the tree it replaces. */
int target_switch_clusters(void)
{
    return target_get() == TARGET_THUMB;
}

int target_switch_table_min_os(void)
{
    return target_get() == TARGET_THUMB && target_thumb_arch() >= 7 ? 4 : 6;
}

static int (*g_calls_helper)(const struct ir_ins *i);
void target_set_calls_helper(int (*pred)(const struct ir_ins *i)) { g_calls_helper = pred; }
int target_op_calls_helper(const struct ir_ins *i)
{
    return g_calls_helper ? g_calls_helper(i) : 0;
}

int target_anon_bitfield_aligns(void)
{
    switch (target_get()) {
    case TARGET_X86_64:  return 0;   /* SysV */
    case TARGET_AARCH64:             /* AAPCS64; Apple's arm64 lays them
                                      * out as x86-64 does, and a struct
                                      * { char a; int :0; char b; } was
                                      * 8 bytes against clang's 5 */
        return !darwin_a64();
    case TARGET_THUMB:   return 1;   /* AAPCS */
    case TARGET_RISCV32:
    case TARGET_RISCV64: return 0;   /* RISC-V psABI */
    case TARGET_AVR:     return 0;   /* moot: every alignment is 1 */
    /* o32: `struct { char c; int :4; char d; }` is 3 bytes in clang, and
     * `int :0` moves d to offset 4 without making the struct 4-aligned */
    case TARGET_MIPS32:  return 0;
    case TARGET_MIPS64:  return 0;   /* n64: as o32 (clang) */
    /* LoongArch psABI: `struct { char c; int :0; char d; }` is 5 bytes in
     * clang, aligned 1 */
    case TARGET_LOONGARCH64: return 0;
    case TARGET_TRICORE: return 0;   /* as GCC lays them out (unverified) */
    /* GCC's generic rule (PCC_BITFIELD_TYPE_MATTERS, no ABI override) */
    case TARGET_XTENSA:  return 0;
    /* SVR4 PowerPC: `struct { char c; int :4; char d; }` is 3 bytes in
     * clang, aligned 1 */
    case TARGET_PPC32:   return 0;
    /* RX: the Microsoft layout, where an unnamed bit-field's type raises
     * the struct's alignment like a named one's (stor-layout.cc) */
    case TARGET_RX:      return 1;
    /* SPARC: `struct { char c; int :4; char d; }` is 3 bytes in clang */
    case TARGET_SPARC32: return 0;
    /* m68k: only named members align a structure (and nothing beyond 2) */
    case TARGET_COLDFIRE: return 0;
    }
    return 0;
}

int target_long_size_types(void) { return g_arch == TARGET_RX; }
int target_ms_bitfields(void)    { return g_arch == TARGET_RX; }

int target_va_list_is_pointer(void)
{
    switch (target_get()) {
    /* SysV: __va_list_tag, 24 bytes; Microsoft x64: char *, walking the
     * caller's slots (the callee spills rcx..r9 into the home area) */
    case TARGET_X86_64:  return target_win64_abi();
    /* AAPCS64: the va_list record, 32 bytes -- but Apple's arm64 has
     * none: its va_list is the walking pointer (irg_va_arg_darwin). Read
     * as a record, va_copy copied 32 bytes OF THE ARGUMENTS, and the
     * copy walked that snapshot: a fifth argument read garbage. */
    case TARGET_AARCH64: return g_os == TGT_OS_DARWIN;
    case TARGET_THUMB:   return 1;   /* AAPCS32: void * */
    case TARGET_RISCV32:
    case TARGET_RISCV64: return 1;   /* RISC-V psABI: void * */
    case TARGET_AVR:     return 1;   /* avr-gcc: char * */
    case TARGET_MIPS32:  return 1;   /* o32: void *, over the home area */
    case TARGET_MIPS64:  return 1;   /* n64: void *, over the save area */
    case TARGET_LOONGARCH64: return 1;   /* LoongArch psABI: void * */
    case TARGET_TRICORE: return 1;   /* char *, over the caller's stack words */
    /* GCC's 12-byte record, held by value -- not a pointer to one, and not
     * a tag va_copy needs: a copy is the record's assignment (irgen) */
    case TARGET_XTENSA:  return 0;
    /* SVR4 PowerPC: a 12-byte record (the GPR count, the FPR count, the
     * overflow area and the register save area) that va_start builds and
     * va_arg advances; `va_list` is a pointer to it, which is what clang's
     * one-element array decays to when it is passed */
    case TARGET_PPC32:   return 0;
    case TARGET_RX:      return 1;   /* char *, over the stacked arguments */
    case TARGET_SPARC32: return 1;   /* void *, over the home area */
    case TARGET_COLDFIRE: return 1;  /* m68k: char *, over the caller's
                                      * argument words */
    }
    return 0;
}

int target_widen_unsigned_fp_cvt(void)
{
    return g_arch == TARGET_X86_64 || g_arch == TARGET_AARCH64;
}

int target_has_os(void) { return g_os != TGT_OS_NONE; }

int target_is_hosted(void)
{
    return g_os == TGT_OS_LINUX || g_os == TGT_OS_DARWIN ||
           g_os == TGT_OS_WINDOWS;
}

const char *target_os_name(enum target_os o)
{
    switch (o) {
    case TGT_OS_EMBLINK: return "emblink";
    case TGT_OS_LINUX:   return "linux";
    case TGT_OS_DARWIN:  return "darwin";
    case TGT_OS_WINDOWS: return "windows";
    default:                return "none";
    }
}

const char *target_fmt_name(enum target_fmt f)
{
    switch (f) {
    case TGT_FMT_MACHO: return "Mach-O";
    case TGT_FMT_COFF:  return "COFF";
    default:               return "ELF";
    }
}

/* Every triple this compiler accepts, and what each one means.
 *
 * Explicit rather than assembled from an architecture list crossed with
 * an OS list, because the cross product contains combinations that do
 * not exist (aarch64-windows-gnu) and combinations this compiler cannot
 * yet write. A table that can only name what is real cannot accidentally
 * accept what is not, and THE RULE is that an unknown target is refused
 * loudly rather than approximated.
 *
 * `canon` marks the spelling the compiler prints back; the rest are
 * aliases it merely accepts, so --version and diagnostics always give
 * one name for one target.
 */
/* ARMv7E-M (Cortex-M4/M7) is the same instruction set as v7-M plus the
 * DSP extension and an optional FPU. It used to be accepted as a name
 * that meant v7-M, which was one silent substitution: -dumpmachine
 * answered `thumbv7m-none-eabi` for a v7em request, and the object's
 * Tag_CPU_arch said v7 where the part is v7E-M. A consumer reading that
 * attribute is told the wrong architecture. So the name now carries
 * state, even though the code generated for the two is still identical
 * -- what differs is what the object SAYS about itself. */
static const struct triple {
    const char *name;
    enum target_arch arch;
    enum target_os os;
    enum target_fmt fmt;
    int canon;         /* 1 the canonical name; 2 the v7E-M one; 3 the v8-M
                        * Mainline one; 6 the ARMv6-M one; 9 the v8-M
                        * Baseline one -- see target_triple_of */
    int thumb_em;      /* 1 ARMv7E-M rather than ARMv7-M; 3 ARMv8-M Mainline;
                        * 6 ARMv6-M; 7 ARMv7-A in ARM state (A32); 9 ARMv8-M
                        * Baseline. On MIPS: 1 big-endian */
} g_triples[] = {
    /* freestanding: bare metal and EmbLinkOS (the default) */
    { "x86_64-elf",        TARGET_X86_64,  TGT_OS_NONE,    TGT_FMT_ELF,   1, 0 },
    { "x86_64",            TARGET_X86_64,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 0 },
    { "x86_64-none-elf",   TARGET_X86_64,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 0 },
    { "aarch64-elf",       TARGET_AARCH64, TGT_OS_NONE,    TGT_FMT_ELF,   1, 0 },
    { "aarch64",           TARGET_AARCH64, TGT_OS_NONE,    TGT_FMT_ELF,   0, 0 },
    { "arm64",             TARGET_AARCH64, TGT_OS_NONE,    TGT_FMT_ELF,   0, 0 },
    { "aarch64-none-elf",  TARGET_AARCH64, TGT_OS_NONE,    TGT_FMT_ELF,   0, 0 },

    /* ARMv7-M, the Cortex-M line. Freestanding is the only thing it can
     * be: a microcontroller has no operating system under the code, so
     * there is no `thumbv7m-linux` row to add later and no hosted
     * spelling of this target that would mean anything. `-none-eabi` is
     * the canonical name because that is what every other toolchain
     * calls it and what a project's existing --target= string will say.
     *
     * v7em (Cortex-M4/M7) is the same instruction set plus DSP and an
     * optional FPU; it is accepted as a name now and will differ from
     * v7m only once -mfpu selects hardware floating point. */
    { "thumbv7m-none-eabi", TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   1, 0 },
    { "thumbv7m",           TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 0 },
    { "thumbv7em-none-eabi",TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   2, 1 },
    { "thumbv7em",          TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 1 },
    { "armv7em-none-eabi",  TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 1 },
    { "armv7m-none-eabi",   TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 0 },
    { "arm-none-eabi",      TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 0 },

    /* ARMv6-M: Cortex-M0, M0+ and M1. The same data model and AAPCS32
     * again, so a level on this target (6); what changes is that the
     * instruction set is Thumb-1 plus BL, MRS, MSR and the barriers, and
     * that an unaligned access faults. ARMv8-M Baseline (Cortex-M23) is a
     * different subset -- it has CBZ, MOVW and the divides -- and stays
     * refused until it is selected for. */
    { "thumbv6m-none-eabi", TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   6, 6 },
    { "thumbv6m",           TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 6 },
    { "armv6m-none-eabi",   TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 6 },

    /* ARMv8-M Baseline: Cortex-M23, the low end of ARMv8-M. ARMv6-M's
     * instruction set and data model plus the divides, the exclusives,
     * MOVW/MOVT, CBZ/CBNZ, B.W and the security extension, so it is
     * level 6 with a flag (g_thumb_v8b): `thumb_em` 9 here, canon 9. */
    { "thumbv8m.base-none-eabi", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF, 9, 9 },
    { "thumbv8m.base",      TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 9 },
    { "armv8m.base-none-eabi", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF,    0, 9 },

    /* ARMv8-M Mainline: Cortex-M33, the RTOS requirements' third target,
     * and the RP2350's core. The same data model and the same AAPCS32 as
     * ARMv7-M, so it is a LEVEL on this target and not a new one (see
     * g_thumb_arch). `thumb_em` is 3 here, which the reader below turns
     * into level 8 -- the column already carried "which architecture
     * variant" and this is one more value of it rather than a second
     * column saying the same thing twice.
     *
     * The DSP extension and the FPU are what -mcpu/-mfpu select, exactly
     * as on v7em; the security extension (TrustZone-M) is -mcmse, for the
     * Secure side, and embld mints the secure gateway veneers. */
    /*                                                          canon, em */
    { "thumbv8m.main-none-eabi", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF, 3, 3 },
    { "thumbv8m.main",      TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 3 },
    { "thumbv8m-none-eabi", TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 3 },
    { "armv8m.main-none-eabi", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF,    0, 3 },

    /* The hard-float spellings, as LLVM and Rust name them: the part's
     * FPU (FPv4-SP-D16 on a Cortex-M4F, FPv5-SP-D16 on a Cortex-M33) and
     * floating point passed in its registers. Canon 4 and 5 are these
     * two, so -dumpmachine and the runtime's directory say which
     * convention a build uses -- a hard-float and a soft-float object do
     * not link, so their runtimes cannot share a name. -mfloat-abi= and
     * -mfpu= still override what the name implies. */
    { "thumbv7em-none-eabihf", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF,    4, 1 },

    /* ARMv7-A in ARM state: a Cortex-A (A7, A8, A9, A15), or a Cortex-R
     * running A32. The same ILP32 data model and AAPCS32 as the Cortex-M
     * levels, so a level of this target and not a new one; what differs
     * is the instruction set the encoder writes (src/arch/thumb/a32.c,
     * docs/internals/arm-a32-plan.md) and that base ARMv7-A has no
     * divide. `armv7-none-eabi` is clang's spelling of the same thing. */
    { "armv7a-none-eabi",   TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   7, 7 },
    { "armv7a",             TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 7 },
    { "armv7-none-eabi",    TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 7 },
    { "armv7a-unknown-none-eabi", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF, 0, 7 },
    /* ...with VFP and the hard-float convention: -mfpu=vfpv3-d16 and
     * -mfloat-abi=hard unless a flag says otherwise (canon 8). */
    { "armv7a-none-eabihf", TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   8, 7 },
    { "armv7a-unknown-none-eabihf", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF, 0, 7 },
    { "thumbv8m.main-none-eabihf", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF, 5, 3 },

    /* RISC-V, bare metal. `-unknown-elf` is the spelling the reference
     * toolchains use and the one a project's existing --target= string
     * will say; the short forms are accepted because everyone writes
     * them. Freestanding only for now, as ARMv7-M is: a hosted RISC-V
     * needs an OS underneath and EmbLinkOS does not run there yet. */
    { "riscv32-unknown-elf", TARGET_RISCV32, TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "riscv32",             TARGET_RISCV32, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "riscv32-elf",         TARGET_RISCV32, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "rv32",                TARGET_RISCV32, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "riscv64-unknown-elf", TARGET_RISCV64, TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "riscv64",             TARGET_RISCV64, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "riscv64-elf",         TARGET_RISCV64, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "rv64",                TARGET_RISCV64, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },

    /* AVR. `avr` is the canonical spelling because that is what every
     * other toolchain calls the target and what a project's --target=
     * string will say; the part is selected with -mmcu=, as avr-gcc and
     * clang both do, and not by a triple per device. Freestanding is the
     * only thing an 8-bit microcontroller can be. */
    { "avr",                 TARGET_AVR,     TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "avr-none-elf",        TARGET_AVR,     TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "avr-elf",             TARGET_AVR,     TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "avr-unknown-none",    TARGET_AVR,     TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },

    /* MIPS32r2, little-endian, o32, soft float: a PIC32's core. Bare metal
     * only, like every microcontroller here. `-none-elf` is the canonical
     * spelling; `-unknown-elf` is clang's, and what a project that already
     * builds with clang will say. */
    { "mipsel-none-elf",     TARGET_MIPS32,  TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "mipsel-unknown-elf",  TARGET_MIPS32,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "mipsel-elf",          TARGET_MIPS32,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "mipsel",              TARGET_MIPS32,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    /* ...and the same core BIG-endian, o32 (mips-none-elf): the byte
     * order is the only difference, and it is a property of the target
     * every phase that lays out or reads memory asks about
     * (target_big_endian; docs/internals/big-endian.md). Canon 2, and a 1
     * in the sub-architecture column, which on MIPS means big-endian. */
    { "mips-none-elf",       TARGET_MIPS32,  TGT_OS_NONE,   TGT_FMT_ELF,   2, 1 },
    { "mips-unknown-elf",    TARGET_MIPS32,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 1 },
    { "mips-elf",            TARGET_MIPS32,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 1 },
    { "mips",                TARGET_MIPS32,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 1 },

    /* MIPS64r2, n64, soft float, either byte order: the same columns as
     * the 32-bit pair, the sub-architecture 1 meaning big-endian. */
    { "mips64el-none-elf",   TARGET_MIPS64,  TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "mips64el-unknown-elf", TARGET_MIPS64, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "mips64el-elf",        TARGET_MIPS64,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "mips64el",            TARGET_MIPS64,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "mips64-none-elf",     TARGET_MIPS64,  TGT_OS_NONE,   TGT_FMT_ELF,   2, 1 },
    { "mips64-unknown-elf",  TARGET_MIPS64,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 1 },
    { "mips64-elf",          TARGET_MIPS64,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 1 },
    { "mips64",              TARGET_MIPS64,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 1 },

    /* LoongArch64, LP64S (soft float), bare metal. `-unknown-elf` is
     * clang's spelling and the canonical one; the short forms are accepted
     * because everyone writes them. */
    { "loongarch64-unknown-elf", TARGET_LOONGARCH64, TGT_OS_NONE, TGT_FMT_ELF, 1, 0 },
    { "loongarch64-none-elf", TARGET_LOONGARCH64, TGT_OS_NONE, TGT_FMT_ELF,  0, 0 },
    { "loongarch64-elf",     TARGET_LOONGARCH64, TGT_OS_NONE, TGT_FMT_ELF,   0, 0 },
    { "loongarch64",         TARGET_LOONGARCH64, TGT_OS_NONE, TGT_FMT_ELF,   0, 0 },

    /* Infineon TriCore 1.6.1 (AURIX TC2xx, and the subset of TC3xx's
     * 1.6.2 it shares), little-endian, soft float. `tricore-elf` is the
     * GNU spelling HighTec's toolchain uses. */
    { "tricore-none-elf",    TARGET_TRICORE, TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "tricore-elf",         TARGET_TRICORE, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "tricore-unknown-elf", TARGET_TRICORE, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "tricore",             TARGET_TRICORE, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },

    /* Xtensa, little-endian, windowed ABI: the ESP32 (LX6) and ESP32-S3
     * (LX7). `-none-elf` is the canonical spelling; the -esp* ones are
     * Espressif's toolchains' names, which a project already built with
     * them will say. One instruction set for all of them (the subset
     * the plan names), so one target. */
    { "xtensa-none-elf",     TARGET_XTENSA,  TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "xtensa-esp32-elf",    TARGET_XTENSA,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "xtensa-esp32s3-elf",  TARGET_XTENSA,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "xtensa-esp-elf",      TARGET_XTENSA,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "xtensa-elf",          TARGET_XTENSA,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "xtensa",              TARGET_XTENSA,  TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    /* 32-bit PowerPC, the embedded EABI, big-endian, soft float: an
     * e500/e200-class core. `-none-eabi` is clang's and GCC's spelling
     * (powerpc-eabi is the GNU target name). Bare metal only. */
    { "powerpc-none-eabi",   TARGET_PPC32,   TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "powerpc-unknown-eabi",TARGET_PPC32,   TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "powerpc-eabi",        TARGET_PPC32,   TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "powerpc",             TARGET_PPC32,   TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "ppc",                 TARGET_PPC32,   TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },

    /* Renesas RX (RXv1: RX600/RX610, RX100/RX200), little-endian. GCC's
     * triple is rx-elf; -none-elf is the canonical spelling here, as for
     * the other bare-metal targets. */
    { "rx-none-elf",         TARGET_RX,      TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "rx-elf",              TARGET_RX,      TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "rx-unknown-elf",      TARGET_RX,      TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "rx",                  TARGET_RX,      TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    /* 32-bit SPARC V8, big-endian, soft float: Gaisler's LEON3. Bare
     * metal only; `sparc-none-elf` is clang's spelling (sparc-elf the GNU
     * target name, sparc-gaisler-elf BCC's). */
    { "sparc-none-elf",      TARGET_SPARC32, TGT_OS_NONE,   TGT_FMT_ELF,   1, 0 },
    { "sparc-unknown-elf",   TARGET_SPARC32, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "sparc-elf",           TARGET_SPARC32, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "sparc-gaisler-elf",   TARGET_SPARC32, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    { "sparc",               TARGET_SPARC32, TGT_OS_NONE,   TGT_FMT_ELF,   0, 0 },
    /* ColdFire: GCC's target name is m68k-elf with -mcpu=5208; EmbCC's
     * m68k is ColdFire ISA_A only (the 68000/68020 family proper is
     * refused by -mcpu). Big-endian, bare metal. */
    { "m68k-none-elf",       TARGET_COLDFIRE, TGT_OS_NONE,  TGT_FMT_ELF,   1, 0 },
    { "m68k-unknown-elf",    TARGET_COLDFIRE, TGT_OS_NONE,  TGT_FMT_ELF,   0, 0 },
    { "m68k-elf",            TARGET_COLDFIRE, TGT_OS_NONE,  TGT_FMT_ELF,   0, 0 },
    { "m68k",                TARGET_COLDFIRE, TGT_OS_NONE,  TGT_FMT_ELF,   0, 0 },

    /* EmbLinkOS: the primary product target (vision §5.2). Its objects
     * are ELF; `embld --embx` turns them into a native image at LINK
     * time, which is why the format column says ELF and not EMBX. */
    { "x86_64-emblink",    TARGET_X86_64,  TGT_OS_EMBLINK,  TGT_FMT_ELF,   1, 0 },
    { "aarch64-emblink",   TARGET_AARCH64, TGT_OS_EMBLINK,  TGT_FMT_ELF,   1, 0 },

    /* hosted: someone else's libc and linker (D-014) */
    { "x86_64-linux-gnu",  TARGET_X86_64,  TGT_OS_LINUX,   TGT_FMT_ELF,   1, 0 },
    { "x86_64-linux",      TARGET_X86_64,  TGT_OS_LINUX,   TGT_FMT_ELF,   0, 0 },
    { "aarch64-linux-gnu", TARGET_AARCH64, TGT_OS_LINUX,   TGT_FMT_ELF,   1, 0 },
    { "aarch64-linux",     TARGET_AARCH64, TGT_OS_LINUX,   TGT_FMT_ELF,   0, 0 },
    { "x86_64-apple-darwin",  TARGET_X86_64,  TGT_OS_DARWIN, TGT_FMT_MACHO, 1, 0 },
    { "x86_64-darwin",        TARGET_X86_64,  TGT_OS_DARWIN, TGT_FMT_MACHO, 0, 0 },
    { "aarch64-apple-darwin", TARGET_AARCH64, TGT_OS_DARWIN, TGT_FMT_MACHO, 1, 0 },
    { "arm64-apple-darwin",   TARGET_AARCH64, TGT_OS_DARWIN, TGT_FMT_MACHO, 0, 0 },
    { "aarch64-darwin",       TARGET_AARCH64, TGT_OS_DARWIN, TGT_FMT_MACHO, 0, 0 },
    { "x86_64-windows-gnu",   TARGET_X86_64,  TGT_OS_WINDOWS, TGT_FMT_COFF, 1, 0 },
    { "x86_64-w64-mingw32",   TARGET_X86_64,  TGT_OS_WINDOWS, TGT_FMT_COFF, 0, 0 },
};
static const int g_ntriples = (int)(sizeof g_triples / sizeof g_triples[0]);

int target_from_triple(const char *triple, enum target_arch *out,
                       enum target_os *os, enum target_fmt *fmt)
{
    for (int i = 0; i < g_ntriples; i++)
        if (strcmp(triple, g_triples[i].name) == 0) {
            if (out) *out = g_triples[i].arch;
            if (os)  *os  = g_triples[i].os;
            if (fmt) *fmt = g_triples[i].fmt;
            /* Byte order travels with the name too: every triple but
             * the big-endian MIPS ones is little-endian. */
            g_big_endian = ((g_triples[i].arch == TARGET_MIPS32 ||
                             g_triples[i].arch == TARGET_MIPS64) &&
                            g_triples[i].thumb_em == 1) ||
                           g_triples[i].arch == TARGET_PPC32 ||
                           g_triples[i].arch == TARGET_SPARC32 ||
                           g_triples[i].arch == TARGET_COLDFIRE;
            /* The ARM sub-architecture travels with the name, so
             * -dumpmachine and the object's Tag_CPU_arch both answer
             * what was ASKED for rather than the base profile. */
            if (g_triples[i].arch == TARGET_THUMB) {
                /* 3 in this column means ARMv8-M Mainline. It implies the
                 * DSP extension too -- v8-M Mainline includes it -- so the
                 * `em` flag stays set for the code that asks "may I use the
                 * v7E-M/DSP instructions". */
                size_t n = strlen(triple);
                g_thumb_hf_name = n > 6 && !strcmp(triple + n - 6, "eabihf");
                g_arm_a32 = g_triples[i].thumb_em == 7;
                g_thumb_v8b = g_triples[i].thumb_em == 9;
                if (g_triples[i].thumb_em == 7) {
                    /* v7-A has the DSP instructions v7E-M adds */
                    g_thumb_arch = 7;
                    g_thumb_em = 1;
                } else if (g_triples[i].thumb_em == 3) {
                    g_thumb_arch = 8;
                    g_thumb_em = 1;
                } else if (g_triples[i].thumb_em == 6 ||
                           g_triples[i].thumb_em == 9) {
                    g_thumb_arch = 6;
                    g_thumb_em = 0;
                } else {
                    g_thumb_arch = 7;
                    g_thumb_em = g_triples[i].thumb_em;
                }
            }
            return 1;
        }
    return 0;
}

const char *target_triple_of(enum target_arch a, enum target_os o)
{
    /* canon 2 is the ARMv7E-M spelling and canon 3 the ARMv8-M Mainline
     * one: the canonical name for this arch/os pair depends on the
     * sub-architecture as well, which is the only place that is true. */
    int want = 1;
    if (a == TARGET_THUMB)
        want = g_arm_a32 ? (g_thumb_hard ? 8 : 7)
             : g_thumb_arch >= 8 ? (g_thumb_hard ? 5 : 3)
             : g_thumb_arch == 6 ? (g_thumb_v8b ? 9 : 6)
             : g_thumb_em ? (g_thumb_hard ? 4 : 2) : 1;
    if ((a == TARGET_MIPS32 || a == TARGET_MIPS64) && g_big_endian)
        want = 2;
    for (int i = 0; i < g_ntriples; i++)
        if (g_triples[i].canon == want && g_triples[i].arch == a &&
            g_triples[i].os == o)
            return g_triples[i].name;
    for (int i = 0; i < g_ntriples; i++)
        if (g_triples[i].canon == 1 && g_triples[i].arch == a &&
            g_triples[i].os == o)
            return g_triples[i].name;
    return NULL;
}

/* ARMv7E-M rather than ARMv7-M: the DSP extension and, on an F part, an
 * FPU. The code generated is identical today -- what differs is what
 * the object reports about itself, which a consumer is entitled to
 * believe. */
int target_thumb_em(void) { return g_thumb_em; }
int target_arm_a32(void) { return g_arch == TARGET_THUMB && g_arm_a32; }
int target_arm_vfp(int *d32)
{
    if (d32)
        *d32 = g_arm_vfp_d32;
    return g_arm_vfp;
}
void target_set_arm_vfp(int version, int d32)
{
    g_arm_vfp = version;
    g_arm_vfp_d32 = d32 ? 1 : 0;
}
int target_thumb_arch(void) { return g_thumb_arch; }
int target_object_align(int is_array, long size, int align)
{
    if (g_arch == TARGET_THUMB && g_thumb_arch == 6 && is_array &&
        size >= 4 && align < 4)
        return 4;
    return align;
}
int target_string_align(int width)
{
    return width > 1 ? width : 1;
}
void target_set_thumb_arch(int lvl)
{
    g_thumb_arch = lvl;
    g_thumb_v8b = 0;
}
int target_thumb_v8m_base(void)
{
    return g_arch == TARGET_THUMB && g_thumb_arch == 6 && g_thumb_v8b;
}
void target_set_thumb_v8m_base(void)
{
    g_thumb_arch = 6;
    g_thumb_v8b = 1;
    g_thumb_em = 0;
}
int target_thumb_v8m(void)
{
    return g_arch == TARGET_THUMB && !g_arm_a32 &&
           (g_thumb_arch >= 8 || (g_thumb_arch == 6 && g_thumb_v8b));
}
int target_thumb_cmse(void) { return target_thumb_v8m() && g_thumb_cmse; }
void target_set_thumb_cmse(int on) { g_thumb_cmse = on ? 1 : 0; }
int target_thumb_fpu(void) { return g_thumb_fpu; }
void target_set_thumb_fpu(int on) { g_thumb_fpu = on ? 1 : 0; }
int target_thumb_fpu_dp(void) { return g_thumb_fpu && g_thumb_fpu_dp; }
void target_set_thumb_fpu_dp(int on) { g_thumb_fpu_dp = on ? 1 : 0; }
int target_thumb_hard_abi(void) { return g_thumb_hard; }
int target_thumb_hf_name(void) { return g_thumb_hf_name; }

int target_pcs_vfp(int pcs, int varargs)
{
    /* A variadic function uses the base standard whatever else is said:
     * the callee cannot know which file an unnamed argument came in. */
    if (varargs || target_get() != TARGET_THUMB)
        return 0;
    return pcs ? pcs == 2 : g_thumb_hard;
}

int target_pcs_differs(int pcs)
{
    return pcs && target_get() == TARGET_THUMB &&
           (pcs == 2) != (g_thumb_hard != 0);
}

void target_set_thumb_hard_abi(int on) { g_thumb_hard = on ? 1 : 0; }
void target_set_thumb_em(int on) { g_thumb_em = on ? 1 : 0; }

const char *target_triple_now(void)
{
    const char *t = target_triple_of(g_arch, g_os);
    return t ? t : "unknown";
}

int target_triple_count(void) { return g_ntriples; }

const char *target_triple_name(int i)
{
    return i >= 0 && i < g_ntriples ? g_triples[i].name : "";
}

int target_elf_machine(enum target_arch a)
{
    switch (a) {
    case TARGET_AARCH64: return EM_AARCH64;
    case TARGET_THUMB:   return EM_ARM;
    case TARGET_RISCV32:
    case TARGET_RISCV64: return EM_RISCV;
    case TARGET_AVR:     return EM_AVR;
    case TARGET_MIPS32:
    case TARGET_MIPS64:  return EM_MIPS;
    case TARGET_LOONGARCH64: return EM_LOONGARCH;
    case TARGET_TRICORE: return EM_TRICORE;
    case TARGET_XTENSA:  return EM_XTENSA;
    case TARGET_PPC32:   return EM_PPC;
    case TARGET_RX:      return EM_RX;
    case TARGET_SPARC32: return EM_SPARC;
    case TARGET_COLDFIRE: return EM_68K;
    default:             return EM_X86_64;
    }
}

/* The C extension. EmbCC has no -march= yet, so this is on for every
 * RISC-V target -- which is what both reference compilers default to
 * (clang's -march for riscv32-unknown-elf is rv32imac) and what every
 * RISC-V microcontroller implements. When -march= exists this becomes the
 * place that reads it, and the predefined macro table (the per-width
 * predef.c, __riscv_c) has to move with it. */
int target_mul_shift_add(long c, int *k, int *neg, int *j)
{
    if (c <= 2)
        return 0;
    *j = 0;
    while (!(c & 1)) { c >>= 1; (*j)++; }
    if (c < 3)
        return 0;
    for (int b = 1; b < 31; b++) {
        if (c == (1L << b) + 1) { *k = b; *neg = 0; return 1; }
        if (c == (1L << b) - 1) { *k = b; *neg = 1; return 1; }
    }
    return 0;
}

int target_riscv_rvc(void)
{
    return 1;
}

unsigned long target_elf_flags(enum target_arch a)
{
    switch (a) {
    case TARGET_THUMB:   return EF_ARM_EABI_VER5;
    case TARGET_RISCV32:
    case TARGET_RISCV64: return target_riscv_rvc() ? EF_RISCV_RVC : 0;
    case TARGET_AVR:     return EF_AVR_ARCH_AVR5;
    /* What clang writes for -mcpu=mips32r2 -mno-abicalls: the delay
     * slots are filled (with nops), the code is not abicalls/PIC. */
    case TARGET_MIPS32:  return EF_MIPS_ARCH_32R2 | EF_MIPS_ABI_O32 |
                                EF_MIPS_NOREORDER;
    /* ...and for mips64r2 n64: no ABI field (ELFCLASS64 is n64) */
    case TARGET_MIPS64:  return EF_MIPS_ARCH_64R2 | EF_MIPS_NOREORDER;
    /* what clang writes for -mabi=lp64s: the soft-float base ABI, object
     * ABI v1 */
    case TARGET_LOONGARCH64: return EF_LOONGARCH_ABI_SOFT_FLOAT |
                                    EF_LOONGARCH_OBJABI_V1;
    case TARGET_TRICORE: return EF_TRICORE_V1_6_1;
    /* What GNU as writes for the ESP32's objects: XT_INSN | XT_LIT */
    case TARGET_XTENSA:  return EF_XTENSA_XT_INSN | EF_XTENSA_XT_LIT;
    /* what GNU as writes for GCC's -m32bit-doubles -mrx-abi */
    case TARGET_RX:      return E_FLAG_RX_ABI;
    /* binutils' ISA_A with the hardware divide, no MAC, no FPU: what the
     * code uses, which every ColdFire core with a divider executes */
    case TARGET_COLDFIRE: return EF_M68K_CF_ISA_A;
    default:             return 0;
    }
}

int target_elf_uses_rel(enum target_arch a)
{
    return a == TARGET_MIPS32;
}

/* The field is a word in the TARGET's order: an o32 object is either. */
static void put_w32(unsigned char *p, unsigned long v)
{
    target_put_uint(p, 4, v);
}

static unsigned long get_w32(const unsigned char *p)
{
    return (unsigned long)target_get_uint(p, 4);
}

int target_rel_put_addend(enum target_arch a, int type, unsigned char *field,
                          long addend)
{
    unsigned long w;
    if (a != TARGET_MIPS32)
        return 0;
    w = get_w32(field);
    switch (type) {
    case R_MIPS_NONE:
        return 1;
    case R_MIPS_32:
        put_w32(field, (unsigned long)addend & 0xffffffffUL);
        return 1;
    case R_MIPS_26:
        /* the field holds a WORD index: A >> 2 */
        put_w32(field, (w & 0xfc000000UL) |
                        (((unsigned long)addend >> 2) & 0x3ffffffUL));
        return 1;
    case R_MIPS_HI16:
        /* AHI, rounded so that AHI << 16 plus the LO16's sign-extended
         * half gives back the whole addend */
        put_w32(field, (w & 0xffff0000UL) |
                        (((unsigned long)(addend + 0x8000) >> 16) & 0xffffUL));
        return 1;
    case R_MIPS_LO16:
        put_w32(field, (w & 0xffff0000UL) |
                        ((unsigned long)addend & 0xffffUL));
        return 1;
    case R_MIPS_PC16:
        put_w32(field, (w & 0xffff0000UL) |
                        (((unsigned long)addend >> 2) & 0xffffUL));
        return 1;
    default:
        return 0;
    }
}

int target_reloc_type(enum target_arch a, enum reloc_kind k)
{
    if (k == RK_TAIL && a != TARGET_THUMB && a != TARGET_AARCH64)
        k = RK_CALL;
    if (a == TARGET_THUMB) {
        switch (k) {
        /* THM_CALL, not CALL: the caller is in Thumb state, so the
         * field is the split 11+11 offset a `bl` encodes there and not
         * the ARM-state 24-bit one. A linker told CALL would patch the
         * wrong bits of the right instruction. */
        /* ARM state (armv7a): the same acts, the A32 instructions' fields */
        case RK_CALL:        return g_arm_a32 ? R_ARM_CALL : R_ARM_THM_CALL;
        case RK_TAIL:        return g_arm_a32 ? R_ARM_JUMP24 : R_ARM_THM_JUMP24;
        case RK_ABS32:       return R_ARM_ABS32;
        case RK_DATA_PREL32: return R_ARM_REL32;
        case RK_THM_MOVW:    return g_arm_a32 ? R_ARM_MOVW_ABS_NC
                                              : R_ARM_THM_MOVW_ABS_NC;
        case RK_THM_MOVT:    return g_arm_a32 ? R_ARM_MOVT_ABS
                                              : R_ARM_THM_MOVT_ABS;
        /* No ABS64: a 32-bit target has no 64-bit address to relocate,
         * and asking for one is a bug upstream rather than a kind this
         * table is merely missing. */
        default:             return -1;
        }
    }
    if (a == TARGET_RISCV32 || a == TARGET_RISCV64) {
        switch (k) {
        /* One relocation for the auipc/jalr PAIR: the linker patches
         * both from this one site. _PLT rather than plain CALL because
         * that is what every RISC-V toolchain emits and what a linker
         * with a PLT needs; one without treats them alike. */
        case RK_CALL:        return R_RISCV_CALL_PLT;
        case RK_RISCV_PCREL_HI20:   return R_RISCV_PCREL_HI20;
        case RK_RISCV_PCREL_LO12_I: return R_RISCV_PCREL_LO12_I;
        case RK_ABS32:       return R_RISCV_32;
        /* ABS64 only at RV64: a 32-bit target has no 64-bit address to
         * relocate, and asking for one is a bug upstream rather than a
         * kind this table merely lacks. */
        case RK_ABS64:       return a == TARGET_RISCV64 ? R_RISCV_64 : -1;
        default:             return -1;
        }
    }
    if (a == TARGET_LOONGARCH64) {
        switch (k) {
        /* bl and b alike: a 26-bit word offset, +-128 MiB -- the normal
         * code model (docs/internals/loongarch64-plan.md) */
        case RK_CALL:          return R_LARCH_B26;
        case RK_LA_PCALA_HI20: return R_LARCH_PCALA_HI20;
        case RK_LA_PCALA_LO12: return R_LARCH_PCALA_LO12;
        case RK_ABS32:         return R_LARCH_32;
        case RK_ABS64:         return R_LARCH_64;
        case RK_DATA_PREL32:   return R_LARCH_32_PCREL;
        default:               return -1;
        }
    }
    if (a == TARGET_MIPS64) {
        switch (k) {
        case RK_CALL:         return R_MIPS_26;
        case RK_MIPS_HIGHEST: return R_MIPS_HIGHEST;
        case RK_MIPS_HIGHER:  return R_MIPS_HIGHER;
        case RK_MIPS_HI16:    return R_MIPS_HI16;
        case RK_MIPS_LO16:    return R_MIPS_LO16;
        case RK_MIPS_TEXT26:  return R_MIPS_26;
        case RK_ABS32:        return R_MIPS_32;
        case RK_ABS64:        return R_MIPS_64;
        default:              return -1;
        }
    }
    if (a == TARGET_MIPS32) {
        switch (k) {
        /* jal and j alike: a 26-bit word index within the 256 MiB region
         * of the delay slot. */
        case RK_CALL:        return R_MIPS_26;
        case RK_MIPS_HI16:   return R_MIPS_HI16;
        case RK_MIPS_LO16:   return R_MIPS_LO16;
        case RK_MIPS_TEXT26: return R_MIPS_26;
        case RK_ABS32:       return R_MIPS_32;
        default:             return -1;
        }
    }
    if (a == TARGET_TRICORE) {
        switch (k) {
        /* CALL and J alike: a halfword displacement in 24 bits */
        case RK_CALL:        return R_TRICORE_24REL;
        case RK_TRICORE_HI:  return R_TRICORE_HIADJ;
        case RK_TRICORE_LO:  return R_TRICORE_LO;
        case RK_TRICORE_LO2: return R_TRICORE_LO2;
        case RK_ABS32:       return R_TRICORE_32ABS;
        default:             return -1;
        }
    }
    if (a == TARGET_XTENSA) {
        switch (k) {
        /* call8's offset, which a linker decodes the opcode to find */
        case RK_CALL:        return R_XTENSA_SLOT0_OP;
        /* a literal-pool word, a data pointer, a DWARF offset */
        case RK_ABS32:       return R_XTENSA_32;
        case RK_XTENSA_TEXT32: return R_XTENSA_32;
        default:             return -1;
        }
    }
    if (a == TARGET_PPC32) {
        switch (k) {
        /* bl and b alike: a 24-bit word displacement from the branch */
        case RK_CALL:          return R_PPC_REL24;
        case RK_PPC_ADDR16_HA: return R_PPC_ADDR16_HA;
        case RK_PPC_ADDR16_LO: return R_PPC_ADDR16_LO;
        case RK_ABS32:         return R_PPC_ADDR32;
        case RK_DATA_PREL32:   return R_PPC_REL32;
        default:               return -1;
        }
    }
    if (a == TARGET_RX) {
        switch (k) {
        /* bsr.a's 24-bit field, measured from its opcode */
        case RK_CALL:        return R_RX_DIR24S_PCREL;
        case RK_ABS32:       return R_RX_DIR32;
        default:             return -1;
        }
    }
    if (a == TARGET_SPARC32) {
        switch (k) {
        /* call: a 30-bit word displacement from the call itself */
        case RK_CALL:        return R_SPARC_WDISP30;
        case RK_SPARC_HI22:  return R_SPARC_HI22;
        case RK_SPARC_LO10:  return R_SPARC_LO10;
        case RK_ABS32:       return R_SPARC_32;
        case RK_DATA_PREL32: return R_SPARC_DISP32;
        default:             return -1;
        }
    }
    if (a == TARGET_COLDFIRE) {
        switch (k) {
        /* jsr and jmp to an absolute address, and every address operand:
         * the 32-bit field in the extension words */
        case RK_CALL:        return R_68K_32;
        case RK_TAIL:        return R_68K_32;
        case RK_ABS32:       return R_68K_32;
        case RK_DATA_PREL32: return R_68K_PC32;
        default:             return -1;
        }
    }
    if (a == TARGET_AVR) {
        switch (k) {
        case RK_CALL:            return R_AVR_CALL;
        case RK_AVR_CALL:        return R_AVR_CALL;
        case RK_AVR_TEXT_CALL:   return R_AVR_CALL;
        case RK_AVR_LO8_LDI:     return R_AVR_LO8_LDI;
        case RK_AVR_HI8_LDI:     return R_AVR_HI8_LDI;
        case RK_AVR_LO8_LDI_GS:  return R_AVR_LO8_LDI_GS;
        case RK_AVR_HI8_LDI_GS:  return R_AVR_HI8_LDI_GS;
        case RK_AVR_ABS16:       return R_AVR_16;
        case RK_AVR_ABS16_PM:    return R_AVR_16_PM;
        /* R_AVR_32 exists and is NOT the pointer relocation: a pointer
         * here is two bytes. It is what a `.long` holding an address
         * would need, and nothing emits one yet. */
        case RK_ABS32:           return R_AVR_32;
        default:                 return -1;
        }
    }
    if (a == TARGET_AARCH64) {
        switch (k) {
        /* CALL26, not JUMP26: the field is the same, but CALL26 is what a
         * `bl` carries, and a linker may only insert a veneer for the
         * range-exceeding case on the call form. */
        case RK_CALL:     return R_AARCH64_CALL26;
        case RK_TAIL:     return R_AARCH64_JUMP26;   /* `b`: a tail call */
        case RK_ADR_HI21: return R_AARCH64_ADR_PREL_PG_HI21;
        case RK_ADD_LO12: return R_AARCH64_ADD_ABS_LO12_NC;
        case RK_ABS64:    return R_AARCH64_ABS64;
        case RK_ABS32:    return R_AARCH64_ABS32;
        case RK_DATA_PREL32: return R_AARCH64_PREL32;
        case RK_GOT_PAGE: return R_AARCH64_ADR_GOT_PAGE;
        case RK_GOT_LO12: return R_AARCH64_LD64_GOT_LO12_NC;
        case RK_TPREL_HI12: return R_AARCH64_TLSLE_ADD_TPREL_HI12;
        case RK_TPREL_LO12: return R_AARCH64_TLSLE_ADD_TPREL_LO12_NC;
        default:          return -1;
        }
    }
    switch (k) {
    /* PLT32 rather than PC32 for calls: it lets the linker route through
     * a PLT entry when the callee turns out to be far away or interposed,
     * and degrades to PC32 when it does not. */
    case RK_CALL:     return R_X86_64_PLT32;
    case RK_PCREL32:  return R_X86_64_PC32;
    case RK_ABS64:    return R_X86_64_64;
    case RK_ABS32:    return R_X86_64_32;
    case RK_DATA_PREL32: return R_X86_64_PC32;
    case RK_TPOFF32:  return R_X86_64_TPOFF32;
    default:          return -1;
    }
}

int target_win64_abi(void)
{
    return g_arch == TARGET_X86_64 && g_os == TGT_OS_WINDOWS;
}

int target_coff_reloc(enum target_arch a, enum reloc_kind k)
{
    /* Windows on aarch64 is not a triple EmbCC offers (D-014 puts MinGW
     * x86-64 first, because it reuses the Itanium C++ ABI this tree
     * already has), so there is no ARM64 table to get wrong. */
    if (a != TARGET_X86_64)
        return -1;
    switch (k) {
    case RK_CALL:     return IMAGE_REL_AMD64_REL32;
    case RK_PCREL32:  return IMAGE_REL_AMD64_REL32;
    case RK_ABS64:    return IMAGE_REL_AMD64_ADDR64;
    case RK_ABS32:    return IMAGE_REL_AMD64_ADDR32;
    default:          return -1;
    }
}

int target_macho_reloc(enum target_arch a, enum reloc_kind k,
                       int *pcrel, int *length)
{
    int t = 0, pc = 0, len = 2;
    if (a == TARGET_THUMB)
        return 0;         /* no Mach-O on a microcontroller, ever */
    if (a == TARGET_AARCH64) {
        switch (k) {
        case RK_CALL:     t = ARM64_RELOC_BRANCH26;  pc = 1; break;
        case RK_ADR_HI21: t = ARM64_RELOC_PAGE21;    pc = 1; break;
        case RK_ADD_LO12: t = ARM64_RELOC_PAGEOFF12; pc = 0; break;
        case RK_GOT_PAGE: t = ARM64_RELOC_GOT_LOAD_PAGE21;    pc = 1; break;
        case RK_GOT_LO12: t = ARM64_RELOC_GOT_LOAD_PAGEOFF12; pc = 0; break;
        case RK_ABS64:    t = ARM64_RELOC_UNSIGNED; pc = 0; len = 3; break;
        case RK_ABS32:    t = ARM64_RELOC_UNSIGNED; pc = 0; break;
        /* RK_DATA_PREL32 is "target minus this address", which Mach-O
         * expresses as a SUBTRACTOR/UNSIGNED PAIR rather than one
         * entry. It is only used by the unwind tables, which this
         * target does not emit yet, so it is refused rather than
         * approximated. */
        default: return 0;
        }
    } else {
        switch (k) {
        case RK_CALL:    t = X86_64_RELOC_BRANCH;   pc = 1; break;
        case RK_PCREL32: t = X86_64_RELOC_SIGNED;   pc = 1; break;
        case RK_ABS64:   t = X86_64_RELOC_UNSIGNED; pc = 0; len = 3; break;
        case RK_ABS32:   t = X86_64_RELOC_UNSIGNED; pc = 0; break;
        default: return 0;
        }
    }
    if (pcrel)  *pcrel = pc;
    if (length) *length = len;
    return t + 1;          /* +1 so type 0 (UNSIGNED) is not "no answer" */
}

long target_reloc_addend(enum target_arch a, enum reloc_kind k, long bias)
{
    if (a == TARGET_AARCH64 || a == TARGET_THUMB ||
        a == TARGET_RISCV32 || a == TARGET_RISCV64 || a == TARGET_AVR ||
        a == TARGET_MIPS32 || a == TARGET_LOONGARCH64 ||
        a == TARGET_TRICORE || a == TARGET_XTENSA || a == TARGET_PPC32 ||
        a == TARGET_RX || a == TARGET_SPARC32 ||
        a == TARGET_COLDFIRE || a == TARGET_MIPS64)
        return bias;              /* ARM and RISC-V fields are relative to
                                   * the instruction itself, so no
                                   * end-of-instruction bias. On RISC-V
                                   * that is the `auipc`, which is where
                                   * the relocation sits and where the pc
                                   * it adds to is measured from. */
    switch (k) {
    case RK_CALL:
    case RK_PCREL32:
        return bias - 4;          /* x86-64 rel32 is measured from the END
                                   * of the instruction, four bytes on */
    default:
        return bias;
    }
}

/* ELF relocation types by name, as an assembler's `.reloc` spells them:
 * the ones `embcc -S` writes for Cortex-M, RISC-V and AVR, and the ones
 * an object from these targets may carry beside them. One table read
 * both ways -- -S writes a name from a number (src/driver/asmout.c) and
 * the assembler reads it back (src/as/gas.c) -- so the two cannot
 * disagree about what a name means. */
static const struct reloc_spelling {
    int machine;
    int type;
    const char *name;
} reloc_names[] = {
    { EM_ARM,   R_ARM_NONE,            "R_ARM_NONE" },
    { EM_ARM,   R_ARM_ABS32,           "R_ARM_ABS32" },
    { EM_ARM,   R_ARM_REL32,           "R_ARM_REL32" },
    { EM_ARM,   R_ARM_THM_CALL,        "R_ARM_THM_CALL" },
    { EM_ARM,   R_ARM_THM_JUMP24,      "R_ARM_THM_JUMP24" },
    { EM_ARM,   R_ARM_TARGET1,         "R_ARM_TARGET1" },
    { EM_ARM,   R_ARM_PREL31,          "R_ARM_PREL31" },
    { EM_ARM,   R_ARM_THM_MOVW_ABS_NC, "R_ARM_THM_MOVW_ABS_NC" },
    { EM_ARM,   R_ARM_THM_MOVT_ABS,    "R_ARM_THM_MOVT_ABS" },
    { EM_RISCV, R_RISCV_32,            "R_RISCV_32" },
    { EM_RISCV, R_RISCV_64,            "R_RISCV_64" },
    { EM_RISCV, R_RISCV_BRANCH,        "R_RISCV_BRANCH" },
    { EM_RISCV, R_RISCV_JAL,           "R_RISCV_JAL" },
    { EM_RISCV, R_RISCV_CALL,          "R_RISCV_CALL" },
    { EM_RISCV, R_RISCV_CALL_PLT,      "R_RISCV_CALL_PLT" },
    { EM_RISCV, R_RISCV_PCREL_HI20,    "R_RISCV_PCREL_HI20" },
    { EM_RISCV, R_RISCV_PCREL_LO12_I,  "R_RISCV_PCREL_LO12_I" },
    { EM_RISCV, R_RISCV_PCREL_LO12_S,  "R_RISCV_PCREL_LO12_S" },
    { EM_RISCV, R_RISCV_HI20,          "R_RISCV_HI20" },
    { EM_RISCV, R_RISCV_LO12_I,        "R_RISCV_LO12_I" },
    { EM_RISCV, R_RISCV_LO12_S,        "R_RISCV_LO12_S" },
    { EM_LOONGARCH, R_LARCH_NONE,      "R_LARCH_NONE" },
    { EM_LOONGARCH, R_LARCH_32,        "R_LARCH_32" },
    { EM_LOONGARCH, R_LARCH_64,        "R_LARCH_64" },
    { EM_LOONGARCH, R_LARCH_B16,       "R_LARCH_B16" },
    { EM_LOONGARCH, R_LARCH_B21,       "R_LARCH_B21" },
    { EM_LOONGARCH, R_LARCH_B26,       "R_LARCH_B26" },
    { EM_LOONGARCH, R_LARCH_PCALA_HI20, "R_LARCH_PCALA_HI20" },
    { EM_LOONGARCH, R_LARCH_PCALA_LO12, "R_LARCH_PCALA_LO12" },
    { EM_LOONGARCH, R_LARCH_CALL36,    "R_LARCH_CALL36" },
    { EM_LOONGARCH, R_LARCH_32_PCREL,  "R_LARCH_32_PCREL" },
    { EM_PPC,   R_PPC_NONE,            "R_PPC_NONE" },
    { EM_PPC,   R_PPC_ADDR32,          "R_PPC_ADDR32" },
    { EM_PPC,   R_PPC_ADDR16_LO,       "R_PPC_ADDR16_LO" },
    { EM_PPC,   R_PPC_ADDR16_HI,       "R_PPC_ADDR16_HI" },
    { EM_PPC,   R_PPC_ADDR16_HA,       "R_PPC_ADDR16_HA" },
    { EM_PPC,   R_PPC_REL24,           "R_PPC_REL24" },
    { EM_PPC,   R_PPC_REL14,           "R_PPC_REL14" },
    { EM_PPC,   R_PPC_REL32,           "R_PPC_REL32" },
    { EM_RX,    R_RX_NONE,             "R_RX_NONE" },
    { EM_RX,    R_RX_DIR32,            "R_RX_DIR32" },
    { EM_RX,    R_RX_DIR24S_PCREL,     "R_RX_DIR24S_PCREL" },
    { EM_RX,    R_RX_DIR16S_PCREL,     "R_RX_DIR16S_PCREL" },
    { EM_RX,    R_RX_DIR8S_PCREL,      "R_RX_DIR8S_PCREL" },
    { EM_SPARC, R_SPARC_NONE,          "R_SPARC_NONE" },
    { EM_SPARC, R_SPARC_32,            "R_SPARC_32" },
    { EM_SPARC, R_SPARC_DISP32,        "R_SPARC_DISP32" },
    { EM_SPARC, R_SPARC_WDISP30,       "R_SPARC_WDISP30" },
    { EM_SPARC, R_SPARC_WDISP22,       "R_SPARC_WDISP22" },
    { EM_SPARC, R_SPARC_HI22,          "R_SPARC_HI22" },
    { EM_SPARC, R_SPARC_LO10,          "R_SPARC_LO10" },
    { EM_68K,   R_68K_NONE,            "R_68K_NONE" },
    { EM_68K,   R_68K_32,              "R_68K_32" },
    { EM_68K,   R_68K_16,              "R_68K_16" },
    { EM_68K,   R_68K_PC32,            "R_68K_PC32" },
    { EM_68K,   R_68K_PC16,            "R_68K_PC16" },
    { EM_AVR,   R_AVR_NONE,            "R_AVR_NONE" },
    { EM_AVR,   R_AVR_32,              "R_AVR_32" },
    { EM_AVR,   R_AVR_7_PCREL,         "R_AVR_7_PCREL" },
    { EM_AVR,   R_AVR_13_PCREL,        "R_AVR_13_PCREL" },
    { EM_AVR,   R_AVR_16,              "R_AVR_16" },
    { EM_AVR,   R_AVR_16_PM,           "R_AVR_16_PM" },
    { EM_AVR,   R_AVR_LO8_LDI,         "R_AVR_LO8_LDI" },
    { EM_AVR,   R_AVR_HI8_LDI,         "R_AVR_HI8_LDI" },
    { EM_AVR,   R_AVR_CALL,            "R_AVR_CALL" },
    { EM_AVR,   R_AVR_LO8_LDI_GS,      "R_AVR_LO8_LDI_GS" },
    { EM_AVR,   R_AVR_HI8_LDI_GS,      "R_AVR_HI8_LDI_GS" },
    { EM_TRICORE, R_TRICORE_NONE,      "R_TRICORE_NONE" },
    { EM_TRICORE, R_TRICORE_32ABS,     "R_TRICORE_32ABS" },
    { EM_TRICORE, R_TRICORE_24REL,     "R_TRICORE_24REL" },
    { EM_TRICORE, R_TRICORE_HIADJ,     "R_TRICORE_HIADJ" },
    { EM_TRICORE, R_TRICORE_LO,        "R_TRICORE_LO" },
    { EM_TRICORE, R_TRICORE_LO2,       "R_TRICORE_LO2" },
    { EM_XTENSA, R_XTENSA_NONE,        "R_XTENSA_NONE" },
    { EM_XTENSA, R_XTENSA_32,          "R_XTENSA_32" },
    { EM_XTENSA, R_XTENSA_ASM_EXPAND,  "R_XTENSA_ASM_EXPAND" },
    { EM_XTENSA, R_XTENSA_SLOT0_OP,    "R_XTENSA_SLOT0_OP" },
};

const char *target_reloc_name(enum target_arch a, int type)
{
    int m = target_elf_machine(a);
    for (size_t i = 0; i < sizeof reloc_names / sizeof reloc_names[0]; i++)
        if (reloc_names[i].machine == m && reloc_names[i].type == type)
            return reloc_names[i].name;
    return NULL;
}

int target_reloc_by_name(enum target_arch a, const char *name, int n)
{
    int m = target_elf_machine(a);
    for (size_t i = 0; i < sizeof reloc_names / sizeof reloc_names[0]; i++)
        if (reloc_names[i].machine == m &&
            strlen(reloc_names[i].name) == (size_t)n &&
            memcmp(reloc_names[i].name, name, (size_t)n) == 0)
            return reloc_names[i].type;
    return -1;
}
