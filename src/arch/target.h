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
    TARGET_THUMB = 2
};

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
int target_ldouble_size(void);    /* 16, or 8 where it is just a double */
int target_char_unsigned(void);   /* plain `char` with no signed/unsigned */
int target_wchar_unsigned(void);  /* wchar_t, which is always int-sized */

/* Whether __int128 exists at all. It does not on a 32-bit target: the
 * type needs a register pair per half and libgcc's __divti3 family is
 * not in the 32-bit multilib, so the front-end refuses it by name
 * rather than lowering something no backend can carry. */
int target_has_int128(void);

/* The selected target. Defaults to x86_64 so every existing command line
 * keeps its meaning; --target= is the only thing that changes it. */
enum target_arch target_get(void);
void target_set(enum target_arch a);

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
    RK_TPREL_LO12 /* aarch64: the low add of the tprel pair */
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

#endif
