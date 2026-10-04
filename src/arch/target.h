/* The target machine EmbCC emits for.
 *
 * EmbLinkOS is two architectures now (myos/docs/ARM64.md), so the compiler
 * is too. One process compiles for one target, chosen by --target= and
 * fixed before the front-end runs: there is no per-function or per-file
 * switching, and nothing below reads the HOST architecture for any reason.
 *
 * Only the phases that genuinely differ consult this — the lexer, parser
 * and most of sema are machine-neutral and must stay that way.
 */
#ifndef EMBCC_TARGET_TARGET_H
#define EMBCC_TARGET_TARGET_H

enum target_arch {
    TARGET_X86_64 = 0,
    TARGET_AARCH64 = 1,
    /* ARMv7-M: Cortex-M3/M4/M7, which execute Thumb-2 and nothing else.
     * Named for the instruction set rather than the architecture family
     * because that is the part the backend encodes, and because there is
     * no A-profile ARM32 target here to be confused with. */
    TARGET_THUMB = 2,
    /* RISC-V, as two targets rather than one: the instruction set is
     * nearly the same at both widths and ONE backend serves them
     * (src/arch/riscv/, parameterised by XLEN), but the DATA MODEL is
     * not, and that is what this enum keys. RV32 is ILP32 and RV64 is
     * LP64, and a single value could not answer for both. */
    TARGET_RISCV32 = 3,
    TARGET_RISCV64 = 4,
    /* AVR: 8-bit, and the first target here that is not a flat 32- or
     * 64-bit register machine. Three things about it are unlike every
     * other target in this enum, and each one costs work elsewhere:
     *
     *  - the registers are EIGHT bits. A 16-bit value needs a pair and a
     *    32-bit value four, so the register allocator hands out runs
     *    rather than registers.
     *  - pointers are SIXTEEN bits, so this is the first target where a
     *    pointer is narrower than a long.
     *  - it is HARVARD: code and data are separate address spaces, and a
     *    pointer to one is not a pointer to the other. A string literal
     *    lives in flash and reaching it is not an ordinary load.
     *
     * `double` is FOUR bytes here, which no other target does, and
     * `char` is UNSIGNED by default -- avr-gcc's documented behaviour,
     * and it differs from clang's AVR target. Both are measured facts
     * rather than recollections; see docs. */
    TARGET_AVR = 5
};

/* The register width in bytes: 4 on RV32, 8 on RV64 and on the other
 * 64-bit targets. The RISC-V backend is written once against this,
 * because an `add` is an `add` at either width and only the loads, the
 * shifts and the W-suffixed forms differ. */
/* The register width IN BITS: 32 or 64. RISC-V's own name for it, and
 * the number __riscv_xlen carries. target_ptr_size() is the same fact in
 * bytes; this exists because the backend's arithmetic reads more clearly
 * against the name the ISA manual uses. */
int target_xlen(void);

/* The operating system the emitted code will run ON, which is a
 * different question from the architecture and was not asked at all
 * until D-014. It decides the predefined macros real system headers
 * branch on, the object format, the calling convention where they
 * differ, and how a program starts.
 *
 * TGT_OS_NONE is the freestanding case -- `x86_64-elf` and
 * `aarch64-elf`, bare metal and EmbLinkOS -- and stays the default, so
 * every command line that worked before D-014 still means what it did.
 */
/* TGT_ rather than the TARGET_ prefix the architecture enum uses: Apple's
 * clang predefines the whole TARGET_OS_* family itself (TargetConditionals),
 * so TARGET_OS_LINUX is already an object-like macro on one of the hosts
 * this compiler is built on, and the obvious spelling will not compile
 * there. */
enum target_os {
    TGT_OS_NONE = 0,       /* freestanding, no OS at all: *-elf */
    TGT_OS_EMBLINK,        /* EmbLinkOS -- the primary product target */
    TGT_OS_LINUX,
    TGT_OS_DARWIN,
    TGT_OS_WINDOWS         /* MinGW flavour; MSVC is not scheduled */
};

/* The container the objects go in. Orthogonal to the architecture --
 * x86-64 has worn all three -- which is why it is its own dimension
 * rather than a property of either of the others. */
enum target_fmt {
    TGT_FMT_ELF = 0,
    TGT_FMT_MACHO,
    TGT_FMT_COFF
};
/* EMBX is deliberately not here. It is a LINK output -- `embld --embx`
 * writes a native, capability-carrying image instead of an ELF
 * executable (D-003) -- and the objects that go into it are ELF like
 * any others. This enum is the container an OBJECT goes in, so EMBX
 * would be a category error in it, and putting it here would make the
 * compiler think it had a fourth object writer to build. */

/* The DATA MODEL: how wide each type is, and which of the ones C leaves
 * to the implementation are signed.
 *
 * These are functions rather than a `== TARGET_AARCH64` test at each
 * site because for two targets they did not have to be. x86-64 and
 * aarch64 are both LP64 and differ in exactly two of these, so "is it
 * aarch64?" was a serviceable stand-in for "is char unsigned?" and for
 * "is long double binary128?" at once. ARMv7-M answers them
 * independently -- it is ILP32, its char is unsigned like aarch64's,
 * and its long double is a plain double -- so each question now has to
 * be asked by name. A new target that gets one of these wrong should
 * fail to compile a _Static_assert, not silently inherit x86-64's
 * answer because it is not aarch64.
 */
int target_ptr_size(void);        /* 8 on LP64, 4 on ILP32 */
int target_long_size(void);       /* likewise; long long is always 8 */
int target_double_size(void);      /* 8, or 4 on AVR */
int target_int_size(void);
/* The most alignment any scalar gets, or 0 for no cap -- 1 on AVR. */
int target_max_scalar_align(void);         /* 4, or 2 on AVR */
/* What the stack pointer is aligned to at every call -- and so the most a
 * frame slot's offset alone can promise an address. */
int target_stack_align(void);
/* Whether the backend lowers IR_SQRT of a `bytes`-wide float to an
 * instruction: SSE and A64 for float and double, a Cortex-M FPU for
 * float. Elsewhere __builtin_sqrt is a call to the libm function. */
int target_has_sqrt(int bytes);
int target_ldouble_size(void);    /* 16, or 8 where it is just a double */
int target_char_unsigned(void);   /* plain `char` with no signed/unsigned */

/* -fsigned-char / -funsigned-char. Each target has a default and this
 * overrides it for the whole compile; -1 restores the default. It is
 * not a preference -- a buffer of plain `char` compares differently
 * either way -- so a build that asks is obeyed. */
void target_set_char_signed(int unsigned_char);
int target_wchar_unsigned(void);  /* wchar_t, which is always int-sized */

/* Whether __int128 exists at all. It does not on a 32-bit target: the
 * type needs a register pair per half and libgcc's __divti3 family is
 * not in the 32-bit multilib, so the front-end refuses it by name
 * rather than lowering something no backend can carry. */
int target_has_int128(void);
int target_jump_tables(void);     /* a dense switch may be a table: not AVR */
/* Does the current backend lower this op to a CALL of a runtime helper
 * (soft-float arithmetic, a 64-bit divide, an __int128 op)? The
 * allocator already knows -- it is the backend's own predicate, handed
 * over in its ra_target -- and an optimizer pass that reasons about what
 * a value crosses must agree with it, or it reasons about the wrong
 * calls. x86-64 and AVR answer 0: the one has no such helpers, the
 * other's allocator does not model them this way. */
struct ir_ins;
int target_op_calls_helper(const struct ir_ins *i);
/* The driver registers the backend's predicate at target selection:
 * src/arch/target.c itself must not name the backends, because the
 * tools that link it alone (embls) link no Thumb or RISC-V codegen.
 * Unregistered, every op is an ordinary one. */
void target_set_calls_helper(int (*pred)(const struct ir_ins *i));
int t_op_calls_helper(const struct ir_ins *i);      /* src/arch/thumb/codegen.c */
int rv_op_calls_helper(const struct ir_ins *i);     /* src/arch/riscv/codegen.c */
int a64_op_calls_helper(const struct ir_ins *i);    /* src/arch/aarch64/codegen.c */

/* Whether an unsigned 32-bit integer is WIDENED to 64 bits before a
 * conversion to or from floating point.
 *
 * x86-64 needs it: cvtsi2sd and cvttsd2si are signed only, there is no
 * unsigned form, so the zero extension into 64 bits IS how an unsigned
 * 32-bit value is converted exactly. aarch64 has ucvtf and fcvtzu and does
 * not strictly need it; it has always done it and a register-to-register
 * widening costs nothing there, so that stays.
 *
 * The three embedded targets convert with a CALL, and libgcc's names come in
 * both signednesses -- __floatunsisf and __fixunssfsi, which the thumb,
 * riscv and avr backends all already emit and which nothing reached. There
 * the widening is not a correctness device, it is a 64-bit software
 * conversion in place of a 32-bit one. On AVR that also pulls
 * lib/rt/avrfpi64.c into the image: 8.5 KB, on a part with 32768 bytes of
 * flash, for a cast a program writes without thinking about it. */
int target_widen_unsigned_fp_cvt(void);

/* Is a va_list a bare POINTER at the next variadic argument, rather than a
 * pointer to a tag that va_start builds?
 *
 * It decides what va_copy is. Where there is a tag -- SysV x86-64's 24-byte
 * __va_list_tag, AAPCS64's 32-byte record -- va_arg advances the tag in
 * place, so a copy needs a tag of its own. Where the va_list is the pointer
 * itself, the copy IS the assignment, and copying "the tag" copies the
 * arguments instead.
 *
 * This used to be an enumeration written inline in irgen -- "thumb, riscv32,
 * riscv64" -- and when AVR arrived nobody added it. So AVR took the x86-64
 * path: a hidden `long[4]` tag, which is SIXTEEN bytes where long is four,
 * and a 24-byte memcpy into it. Eight bytes ran over whatever the frame
 * layout had put next. At -O0 `copied(1, 7)` returned 7 instead of 707 and
 * the program carried on printing as if nothing were wrong.
 *
 * So it is a switch here with NO default: -Wswitch under -Werror refuses to
 * build until a new target says which it is. */
int target_va_list_is_pointer(void);

/* Does an UNNAMED bit-field -- `unsigned :4;`, `int :0;` -- raise the
 * alignment of the struct it is in? AAPCS and AAPCS64 say yes; x86-64 SysV
 * and the RISC-V psABI say no, only named members count. Measured against
 * x86_64-elf-gcc, aarch64-elf-gcc and clang for Thumb and RISC-V.
 *
 * EmbCC applied ARM's answer everywhere, so on x86-64 and RISC-V
 * `struct { char c; unsigned :4; char d; }` was four bytes where the ABI makes
 * it three -- and `unsigned :4; // reserved` is how a hardware register block
 * is written. A switch with no default, as target_va_list_is_pointer is. */
int target_anon_bitfield_aligns(void);

/* ARMv7E-M (Cortex-M4/M7) rather than ARMv7-M (Cortex-M3). Set by the
 * --target= name and by -mcpu=. The instruction selection is the same
 * for both; this changes what the object SAYS it was built for, which
 * is what a linker and a debugger read. */
int target_thumb_em(void);
/* The Thumb architecture level: 7 (ARMv7-M) or 8 (ARMv8-M Mainline). A
 * level rather than a separate enum target_arch value, because that enum
 * keys the data model and these two share one; see g_thumb_arch. */
int target_thumb_arch(void);
void target_set_thumb_arch(int lvl);
void target_set_thumb_em(int on);

/* Hardware floating point on ARMv7E-M (FPv4-SP-D16, the Cortex-M4F
 * unit): SINGLE precision only, so `float` runs on the FPU and `double`
 * still goes through __adddf3.
 *
 * Separate from the ABI on purpose. This says the compiler may EMIT VFP
 * arithmetic; where floating-point ARGUMENTS travel is a different
 * question (-mfloat-abi), and answering the two together is how an
 * object ends up claiming an ABI it does not implement. -mfpu= stays
 * refused until both halves are right. */
int target_thumb_fpu(void);
void target_set_thumb_fpu(int on);

/* The float ABI, which -mfloat-abi= selects and which is independent of the
 * FPU above:
 *
 *   soft    no FPU instructions; float travels in the core registers
 *   softfp  FPU instructions; float STILL travels in the core registers
 *           (the base standard -- links with soft-float objects)
 *   hard    FPU instructions; float travels in s0-s15 (AAPCS-VFP --
 *           links only with other hard-float objects)
 *
 * target_thumb_fpu() is true for softfp and hard; this is true for hard
 * alone. It decides Tag_ABI_VFP_args and __ARM_PCS_VFP, and it is what the
 * linker checks before mixing two objects. */
int target_thumb_hard_abi(void);
/* Does a function declared __attribute__((pcs(N))) (1 "aapcs", 2
 * "aapcs-vfp", 0 none) use a convention OTHER than this build's? */
int target_pcs_differs(int pcs);
/* Was the target named with an -eabihf triple (thumbv7em-none-eabihf)?
 * The driver reads it as -mfpu=<the part's> -mfloat-abi=hard. */
int target_thumb_hf_name(void);

/* Thumb's answer to "may the optimizer fold this constant into op's
 * immediate operand" (arch/thumb/codegen.c). Asked only by the optimizer
 * (opt.c), which is linked only into embcc: target.c is also linked into
 * the standalone encoding checkers, which carry no backend, so it must
 * not name one. */
int thumb_imm_foldable(int op, long imm);
int thumb_imm_foldable64(int op, long imm);   /* a 64-bit AND/OR/XOR, half by half */
/* Is c == ((1 << k) + 1) << j or ((1 << k) - 1) << j, with k >= 1? Then a
 * multiply by c is an add or a reverse-subtract with a shifted operand,
 * and a shift: one or two instructions on a machine with shifted
 * operands. Powers of two are not here -- the optimizer made those
 * shifts already -- and neither are 0, 1 and 2. */
int target_mul_shift_add(long c, int *k, int *neg, int *j);
int riscv_imm_foldable(int op, long imm);   /* arch/riscv/irgen.c */
int a64_imm_foldable(int op, long imm, int w);   /* arch/aarch64/irgen.c */
/* Are floating-point arguments and results in VFP registers for a
 * function with this pcs and variadic-ness? */
int target_pcs_vfp(int pcs, int varargs);
void target_set_thumb_hard_abi(int on);

/* The selected target. Defaults to x86_64 so every existing command line
 * keeps its meaning; --target= is the only thing that changes it. */
enum target_arch target_get(void);
void target_set(enum target_arch a);
/* -Os, for the one question a backend asks of it: how far to align a
 * function's start. Everything else about -Os is the optimizer's -- the
 * backends read opt_level, which -Os leaves an ordinary number. */
void target_set_opt_size(int on);
int  target_opt_size(void);

/* The other two dimensions. Both default to the freestanding ELF answer,
 * so a caller that has never heard of them reads the world exactly as it
 * was before D-014 -- which is why this went in as an addition rather
 * than as a change to target_get()'s meaning. */
enum target_os  target_os_get(void);
enum target_fmt target_fmt_get(void);
void target_os_set(enum target_os o);
void target_fmt_set(enum target_fmt f);

/* Is there an operating system under this target at all? True for
 * EmbLinkOS as much as for Linux -- both have syscalls, a libc and a
 * process to start. */
int target_has_os(void);

/* Is this a target whose libc, startup objects and linker belong to the
 * PLATFORM rather than to us (D-014)?
 *
 * EmbLinkOS has an operating system and is still not "hosted" in this
 * sense, and the distinction is the whole reason there are two
 * predicates. On Linux the right answer is to use glibc's headers, crt1
 * and ld, because they are there and they are what every other program
 * links against. On EmbLinkOS the right answer is lib/libc over
 * os/emblinkos/backend.c and EmbLD, because those ARE the platform's --
 * we wrote them (D-009). Asking "does it have an OS" and getting back
 * "then use the system toolchain" would send the primary product target
 * looking for a glibc that does not exist. */
int target_is_hosted(void);

/* For diagnostics that must name the triple rather than guess at it. */
const char *target_os_name(enum target_os o);
const char *target_fmt_name(enum target_fmt f);

/* Apply the CONFIGURED default target, before any --target= is seen.
 *
 * GCC's `./configure --target=` builds a compiler that defaults to one
 * machine, so a person cross-compiling for a board types `gcc main.c`
 * and not `gcc --target=... main.c` a hundred times a day. This is that,
 * without a configure script:
 *
 *   --target=                     on the command line, always wins
 *   EMBCC_DEFAULT_TARGET          in the environment, for one shell
 *   -DEMBCC_DEFAULT_TARGET="..."  compiled in (make DEFAULT_TARGET=...)
 *   x86_64-elf                    when none of the above says otherwise
 *
 * NOT `EMBCC_TARGET`, which is taken: tests/lib.sh uses that name for
 * which target the SUITE is exercising, and the suite exports it while
 * passing --target= explicitly. Honouring it here would mean
 * `make test-arm64` silently retargeted every test that relies on the
 * default -- two unrelated things behind one name, and the failure
 * would look like a miscompile.
 *
 * Returns 1, or 0 with the offending name in *bad when a configured
 * triple is not one this compiler knows. */
int target_apply_default(const char **bad);

/* The configured default's triple, or NULL when none was configured and
 * x86_64-elf stands. Not validated -- target_apply_default does that.
 * --help uses it, which is the point: a cross compiler whose help says
 * "x86_64-elf (the default)" is lying to the person reading it. */
const char *target_default_name(void);

/* The length in bytes of the instruction at `p`, for a reader walking a
 * .text stream (-S, and anything else that must group bytes by
 * instruction). Returns 0 when this target's length cannot be decided
 * from the bytes alone -- x86-64, where the caller must ask a real
 * disassembler.
 *
 * This exists because -S used to call the x86-64 disassembler for EVERY
 * target, so a RISC-V .text was grouped into x86 instruction lengths and
 * annotated with x86 mnemonics. The fixed-width targets do not need a
 * disassembler to be grouped correctly -- they need three lines of
 * arithmetic each -- and getting the GROUPING right is what makes the
 * emitted text reassemble to the same object.
 *
 * `avail` is how many bytes remain; a result is never larger than it. */
int target_insn_len(const unsigned char *p, int avail);

/* Parses a full triple into all three dimensions. Returns 0 and leaves
 * every output alone on anything it does not know, so the driver can
 * refuse loudly rather than silently emit for the wrong machine (THE
 * RULE) -- and "does not know" now includes a combination this compiler
 * cannot yet write, which is why the table is explicit rather than
 * assembled from parts. */
int target_from_triple(const char *triple, enum target_arch *out,
                       enum target_os *os, enum target_fmt *fmt);

/* The canonical triple for a combination, for --version and diagnostics.
 * Returns NULL for one that has no canonical name. */
const char *target_triple_of(enum target_arch a, enum target_os o);

/* The canonical triple of what is currently selected. */
const char *target_triple_now(void);

/* Every triple this compiler accepts, for --help and for the error
 * message that lists them. Returns the count; name[i] is the i-th. */
int target_triple_count(void);
const char *target_triple_name(int i);

/* Machine-neutral relocation kinds.
 *
 * Codegen records a KIND at each patch site and the driver turns
 * (kind, target) into an ELF relocation type. The indirection exists
 * because the same source-level act costs a different number of
 * relocations per machine: taking a symbol's address is one RIP-relative
 * `lea` on x86-64, but an `adrp`/`add` PAIR on aarch64 — two sites, two
 * relocations, one address.
 */
enum reloc_kind {
    RK_CALL,      /* direct call to a function symbol */
    RK_PCREL32,   /* x86-64: the rel32 field of a RIP-relative lea */
    RK_ADR_HI21,  /* aarch64: adrp's 21-bit page-relative field */
    RK_ADD_LO12,  /* aarch64: the paired add's 12-bit in-page field */
    RK_ABS64,     /* an absolute 64-bit pointer slot in .data */
    RK_ABS32,     /* an absolute 32-bit field (DWARF section offsets) */
    RK_DATA_PREL32, /* a 32-bit field holding target - its own address
                   * (unwind tables' pointers) */
    RK_GOT_PAGE,  /* aarch64: adrp to the page of the symbol's GOT slot */
    RK_GOT_LO12,  /* aarch64: the paired ldr's offset in that page — a
                   * weak symbol's address (0 when it is undefined, which
                   * adrp/add cannot give) */
    /* Local-exec thread-local storage: the object's offset from the
     * thread pointer. The compiler cannot compute it -- it depends on
     * how large the WHOLE program's thread block turns out to be, which
     * only the linker knows -- so the field is left to a relocation
     * exactly as an address would be. */
    RK_TPOFF32,   /* x86-64: the disp32 of `lea off(%fs-base), reg` */
    RK_TPREL_HI12,/* aarch64: the high add of the tprel pair */
    RK_TPREL_LO12,/* aarch64: the low add of the tprel pair */
    /* ARMv7-M takes a symbol's address in two halves, like aarch64's
     * adrp/add pair and for the same reason: no 32-bit instruction
     * carries a 32-bit operand. `movw` takes the low halfword and
     * `movt` the high one, and each is its own relocation because each
     * patches a different instruction. _NC on the low half — it
     * legitimately drops the bits the high half carries, so a checked
     * form would reject every address above 65535. */
    RK_THM_MOVW,
    RK_THM_MOVT,
    /* RISC-V takes a symbol's address in two halves as well, and the
     * split is arithmetic rather than bitwise: `auipc` supplies bits
     * 31:12 of a PC-RELATIVE displacement and the paired `addi` a
     * SIGN-EXTENDED low 12. So the high half is not simply the top bits
     * -- when bit 11 is set the low half contributes -4096..-1 and the
     * high half must be one larger. Every RISC-V toolchain has that
     * +0x800 and every one that omits it is wrong by 4096 for half of
     * all addresses. The linker does the rounding, because only the
     * linker knows the address.
     *
     * PC-RELATIVE and not absolute, which is not a preference: `lui`
     * SIGN-EXTENDS bit 31, so at RV64 the absolute pair can reach
     * 0..0x7fffffff and 0xffffffff80000000..-1 and nothing between.
     * A firmware image at 0x80000000 -- which is where QEMU's `virt`
     * board and most RISC-V hardware put RAM -- is in the gap, and
     * every address it materialised came out sign-extended and faulted
     * on first use. auipc has no such hole and is position-independent
     * besides, so it is used at BOTH widths rather than keeping a
     * second code model alive for RV32 alone.
     *
     * The LO12 half's relocation names the AUIPC, not the target: the
     * psABI resolves it by looking up the high half's own relocation at
     * the address its symbol gives. The driver emits it against the
     * .text section symbol with the auipc's offset as the addend, which
     * is how an assembler's `.Lpcrel_hi0` label resolves too. */
    RK_RISCV_PCREL_HI20,
    RK_RISCV_PCREL_LO12_I,
    /* AVR. Two things make this target's relocations unlike the others'.
     *
     * An address is materialised a BYTE at a time, because the registers
     * are eight bits wide: `ldi rlo, lo8(sym)` and `ldi rhi, hi8(sym)`
     * are two instructions and two relocations for one sixteen-bit
     * address, and the linker writes a different byte of the same value
     * into each.
     *
     * And program space is a SEPARATE address space, addressed in WORDS.
     * A function pointer therefore holds half a byte address, which is
     * why the _GS and _PM forms exist and why using the data forms for a
     * function would produce a pointer that calls the wrong place --
     * twice as far in, and still a valid instruction when it got there.
     * (_GS is "generate stub": the linker may insert a trampoline when
     * the target is beyond the word-address range, which is why it is the
     * form to use for a function rather than a plain halving.) */
    RK_AVR_CALL,        /* the 22-bit word address of a 32-bit call/jmp */
    RK_AVR_LO8_LDI,     /* ldi: bits 7:0 of a DATA address */
    RK_AVR_HI8_LDI,     /* ldi: bits 15:8 of a data address */
    RK_AVR_LO8_LDI_GS,  /* ldi: bits 7:0 of a FUNCTION's word address */
    RK_AVR_HI8_LDI_GS,  /* ldi: bits 15:8 of a function's word address */
    /* A label inside this object's own .text, for a jump too far for rjmp.
     * AVR has no PC-relative long jump: `jmp` carries an ABSOLUTE word
     * address, which a relocatable object cannot know. So the site is
     * relocated against the .text section symbol with the label's offset as
     * the addend -- the same shape RISC-V's PCREL_LO12 uses, and the reason
     * a 12-bit rjmp is not the whole story on a 32 KB part. */
    RK_AVR_TEXT_CALL,
    RK_AVR_ABS16,       /* a 16-bit data pointer in .data */
    RK_AVR_ABS16_PM,    /* a 16-bit FUNCTION pointer in .data (word) */
    /* A call is `auipc ra, 0` + `jalr ra`, and ONE relocation at the
     * auipc patches BOTH -- which is why there is no separate kind for
     * the jalr. That is the ABI's own shape, not a convenience here. */
    RK_RISCV_CALL,
    /* A TAIL call to a function symbol: a branch, not a call. Thumb
     * spells it differently -- THM_JUMP24 for `b.w` against THM_CALL for
     * `bl`, whose encodings differ in one bit a linker must not flip --
     * and so does aarch64, JUMP26 for `b`; every other target relocates
     * it exactly as RK_CALL. */
    RK_TAIL
};

/* The ELF relocation type for this kind on this target, or -1 if the kind
 * does not apply to it (which is a codegen bug, not an input error).
 * Mach-O and COFF have their own numbering; those mappings arrive with
 * their writers, keyed on (kind, arch, format) as this is on (kind,
 * arch). */
int target_reloc_type(enum target_arch a, enum reloc_kind k);

/* How a kind is spelled as a Mach-O relocation: the type, whether the
 * field is PC-relative, and the log2 of its width. Returns 0 when the
 * kind has no Mach-O spelling yet, which is a refusal the caller must
 * make loudly rather than guess past.
 *
 * The ADDEND is the part that does not carry over and is the easiest
 * thing here to get quietly wrong. ELF's RELA holds it explicitly, and
 * target_reloc_addend() biases x86-64's PC-relative kinds by -4 because
 * the field is measured from the END of the instruction. Mach-O has no
 * addend field: the value sits in the patched word, and the linker
 * already accounts for the instruction's length itself. Biasing it
 * again would move every string reference four bytes -- so the Mach-O
 * path uses the UNBIASED offset, and this comment is why.
 */
/* The COFF relocation type for this kind, or -1 where COFF has none.
 * COFF carries no addend -- like Mach-O and unlike ELF -- so the caller
 * writes it into the field being relocated. Its REL32 is also measured
 * from the END of the instruction rather than from the field, which is
 * what an x86 rel32 means anyway, so the -4 that ELF needs is absent
 * and passing it on would displace every call by four bytes. */
int target_coff_reloc(enum target_arch a, enum reloc_kind k);

/* True where x86-64 uses the MICROSOFT x64 calling convention rather
 * than System V's. This is a property of the OS, not of the
 * architecture -- which is the whole reason D-011's "one architecture,
 * one convention" does not hold any more and the question has to be
 * asked by name.
 *
 * What differs, all of it (checked against clang --target=
 * x86_64-windows-gnu, which is the referee tests/golden/win-abi.sh
 * uses):
 *
 *   Four argument slots, rcx/rdx/r8/r9, and the index is SHARED with
 *   the float registers -- f(int, double, int) is rcx, xmm1, r8, not
 *   rcx, xmm0, rdx. A per-class counter is the single most likely way
 *   to get this wrong, because it produces working code for every
 *   argument list that is all one class.
 *
 *   The caller reserves 32 bytes of SHADOW SPACE below the return
 *   address, which the callee may use to spill its register
 *   arguments. So the first stack argument is at rbp+48, not rbp+16.
 *
 *   A struct is passed by value only when its size is exactly 1, 2, 4
 *   or 8 bytes. Anything else goes BY REFERENCE, and the caller must
 *   pass a pointer to a copy it made, because the callee may write to
 *   it.
 *
 *   A variadic floating-point argument travels in its xmm register
 *   AND in the integer register of the same slot, since the callee
 *   does not know which to read.
 *
 *   rsi and rdi are CALLEE-saved.
 */
int target_win64_abi(void);

int target_macho_reloc(enum target_arch a, enum reloc_kind k,
                       int *pcrel, int *length);

/* The addend the kind carries. x86-64's PC-relative fields are measured
 * from the END of the instruction, so they bias by -4; aarch64's are
 * measured from the instruction itself and bias by 0. `bias` is the
 * site-specific part (a string's offset into .rodata, say). */
long target_reloc_addend(enum target_arch a, enum reloc_kind k, long bias);

/* ELF e_machine. */
int target_elf_machine(enum target_arch a);
/* ...and its e_flags: ARM's EABI version, RISC-V's EF_RISCV_RVC when the
 * C extension is on, AVR's architecture. */
unsigned long target_elf_flags(enum target_arch a);
/* Does RISC-V code use the C extension? The one answer the code generator
 * and the object's e_flags both read. */
int target_riscv_rvc(void);

#endif
