#include "target.h"

#include "../platform/platform.h"

#include <string.h>

#include "../elf/elf.h"
#include "../coff/coff.h"
#include "../macho/macho.h"

static enum target_arch g_arch = TARGET_X86_64;
static int g_thumb_em;      /* --target=thumbv7em-*: see target_thumb_em */
/* The Thumb architecture LEVEL: 7 for ARMv7-M and 8 for ARMv8-M Mainline
 * (Cortex-M33, which the RTOS requirements name as its third target).
 *
 * A level and not a new enum target_arch value, because that enum keys the
 * DATA MODEL -- D-016's reasoning for RISC-V being two targets -- and
 * ARMv8-M's is identical to ARMv7-M's: ILP32, the same sizes, the same
 * AAPCS32. What differs is the instruction set's CEILING (v8-M Mainline is
 * a superset), the predefined macros, and two .ARM.attributes tags. A
 * second enum value would duplicate a data model to express none of that. */
static int g_thumb_arch = 7;
static int g_thumb_fpu;     /* see target_thumb_fpu */
static int g_thumb_hard;    /* see target_thumb_hard_abi */
static enum target_os   g_os   = TGT_OS_NONE;
static enum target_fmt  g_fmt  = TGT_FMT_ELF;

enum target_arch target_get(void) { return g_arch; }
void target_set(enum target_arch a) { g_arch = a; }

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
};

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
int target_ldouble_size(void)   { return g_model[g_arch].ldbl; }
static int g_char_uns_override = -1;

void target_set_char_signed(int unsigned_char)
{
    g_char_uns_override = unsigned_char;
}

int target_char_unsigned(void)
{
    return g_char_uns_override >= 0 ? g_char_uns_override
                                    : g_model[g_arch].char_uns;
}
int target_wchar_unsigned(void) { return g_model[g_arch].wchar_uns; }
int target_has_int128(void)     { return g_model[g_arch].int128; }

int target_anon_bitfield_aligns(void)
{
    switch (target_get()) {
    case TARGET_X86_64:  return 0;   /* SysV */
    case TARGET_AARCH64: return 1;   /* AAPCS64 */
    case TARGET_THUMB:   return 1;   /* AAPCS */
    case TARGET_RISCV32:
    case TARGET_RISCV64: return 0;   /* RISC-V psABI */
    case TARGET_AVR:     return 0;   /* moot: every alignment is 1 */
    }
    return 0;
}

int target_va_list_is_pointer(void)
{
    switch (target_get()) {
    case TARGET_X86_64:  return 0;   /* SysV: __va_list_tag, 24 bytes */
    case TARGET_AARCH64: return 0;   /* AAPCS64: the va_list record, 32 */
    case TARGET_THUMB:   return 1;   /* AAPCS32: void * */
    case TARGET_RISCV32:
    case TARGET_RISCV64: return 1;   /* RISC-V psABI: void * */
    case TARGET_AVR:     return 1;   /* avr-gcc: char * */
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
                        * Mainline one -- see target_triple_of */
    int thumb_em;      /* 1 ARMv7E-M rather than ARMv7-M; 3 ARMv8-M Mainline */
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

    /* ARMv8-M Mainline: Cortex-M33, the RTOS requirements' third target,
     * and the RP2350's core. The same data model and the same AAPCS32 as
     * ARMv7-M, so it is a LEVEL on this target and not a new one (see
     * g_thumb_arch). `thumb_em` is 3 here, which the reader below turns
     * into level 8 -- the column already carried "which architecture
     * variant" and this is one more value of it rather than a second
     * column saying the same thing twice.
     *
     * The DSP extension and the FPU are what -mcpu/-mfpu select, exactly
     * as on v7em; the security extension (TrustZone-M) is refused by name
     * because an object that used it would need the linker to place a
     * secure gateway veneer, which embld does not mint. */
    /*                                                          canon, em */
    { "thumbv8m.main-none-eabi", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF, 3, 3 },
    { "thumbv8m.main",      TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 3 },
    { "thumbv8m-none-eabi", TARGET_THUMB,  TGT_OS_NONE,    TGT_FMT_ELF,   0, 3 },
    { "armv8m.main-none-eabi", TARGET_THUMB, TGT_OS_NONE, TGT_FMT_ELF,    0, 3 },

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
            /* The ARM sub-architecture travels with the name, so
             * -dumpmachine and the object's Tag_CPU_arch both answer
             * what was ASKED for rather than the base profile. */
            if (g_triples[i].arch == TARGET_THUMB) {
                /* 3 in this column means ARMv8-M Mainline. It implies the
                 * DSP extension too -- v8-M Mainline includes it -- so the
                 * `em` flag stays set for the code that asks "may I use the
                 * v7E-M/DSP instructions". */
                if (g_triples[i].thumb_em == 3) {
                    g_thumb_arch = 8;
                    g_thumb_em = 1;
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
        want = g_thumb_arch >= 8 ? 3 : g_thumb_em ? 2 : 1;
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
int target_thumb_arch(void) { return g_thumb_arch; }
void target_set_thumb_arch(int lvl) { g_thumb_arch = lvl; }
int target_thumb_fpu(void) { return g_thumb_fpu; }
void target_set_thumb_fpu(int on) { g_thumb_fpu = on ? 1 : 0; }
int target_thumb_hard_abi(void) { return g_thumb_hard; }
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
    default:             return EM_X86_64;
    }
}

int target_reloc_type(enum target_arch a, enum reloc_kind k)
{
    if (a == TARGET_THUMB) {
        switch (k) {
        /* THM_CALL, not CALL: the caller is in Thumb state, so the
         * field is the split 11+11 offset a `bl` encodes there and not
         * the ARM-state 24-bit one. A linker told CALL would patch the
         * wrong bits of the right instruction. */
        case RK_CALL:        return R_ARM_THM_CALL;
        case RK_ABS32:       return R_ARM_ABS32;
        case RK_DATA_PREL32: return R_ARM_REL32;
        case RK_THM_MOVW:    return R_ARM_THM_MOVW_ABS_NC;
        case RK_THM_MOVT:    return R_ARM_THM_MOVT_ABS;
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
        a == TARGET_RISCV32 || a == TARGET_RISCV64 || a == TARGET_AVR)
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
