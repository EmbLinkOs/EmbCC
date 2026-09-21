#include "target.h"

#include <string.h>

#include "../elf/elf.h"

static enum target_arch g_arch = TARGET_X86_64;
static enum target_os   g_os   = TGT_OS_NONE;
static enum target_fmt  g_fmt  = TGT_FMT_ELF;

enum target_arch target_get(void) { return g_arch; }
void target_set(enum target_arch a) { g_arch = a; }

enum target_os  target_os_get(void)  { return g_os; }
enum target_fmt target_fmt_get(void) { return g_fmt; }
void target_os_set(enum target_os o)   { g_os = o; }
void target_fmt_set(enum target_fmt f) { g_fmt = f; }

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
static const struct triple {
    const char *name;
    enum target_arch arch;
    enum target_os os;
    enum target_fmt fmt;
    int canon;
} g_triples[] = {
    /* freestanding: bare metal and EmbLinkOS (the default) */
    { "x86_64-elf",        TARGET_X86_64,  TGT_OS_NONE,    TGT_FMT_ELF,   1 },
    { "x86_64",            TARGET_X86_64,  TGT_OS_NONE,    TGT_FMT_ELF,   0 },
    { "x86_64-none-elf",   TARGET_X86_64,  TGT_OS_NONE,    TGT_FMT_ELF,   0 },
    { "aarch64-elf",       TARGET_AARCH64, TGT_OS_NONE,    TGT_FMT_ELF,   1 },
    { "aarch64",           TARGET_AARCH64, TGT_OS_NONE,    TGT_FMT_ELF,   0 },
    { "arm64",             TARGET_AARCH64, TGT_OS_NONE,    TGT_FMT_ELF,   0 },
    { "aarch64-none-elf",  TARGET_AARCH64, TGT_OS_NONE,    TGT_FMT_ELF,   0 },

    /* EmbLinkOS: the primary product target (vision §5.2). Its objects
     * are ELF; `embld --embx` turns them into a native image at LINK
     * time, which is why the format column says ELF and not EMBX. */
    { "x86_64-emblink",    TARGET_X86_64,  TGT_OS_EMBLINK,  TGT_FMT_ELF,   1 },
    { "aarch64-emblink",   TARGET_AARCH64, TGT_OS_EMBLINK,  TGT_FMT_ELF,   1 },

    /* hosted: someone else's libc and linker (D-014) */
    { "x86_64-linux-gnu",  TARGET_X86_64,  TGT_OS_LINUX,   TGT_FMT_ELF,   1 },
    { "x86_64-linux",      TARGET_X86_64,  TGT_OS_LINUX,   TGT_FMT_ELF,   0 },
    { "aarch64-linux-gnu", TARGET_AARCH64, TGT_OS_LINUX,   TGT_FMT_ELF,   1 },
    { "aarch64-linux",     TARGET_AARCH64, TGT_OS_LINUX,   TGT_FMT_ELF,   0 },
    { "x86_64-apple-darwin",  TARGET_X86_64,  TGT_OS_DARWIN, TGT_FMT_MACHO, 1 },
    { "x86_64-darwin",        TARGET_X86_64,  TGT_OS_DARWIN, TGT_FMT_MACHO, 0 },
    { "aarch64-apple-darwin", TARGET_AARCH64, TGT_OS_DARWIN, TGT_FMT_MACHO, 1 },
    { "arm64-apple-darwin",   TARGET_AARCH64, TGT_OS_DARWIN, TGT_FMT_MACHO, 0 },
    { "aarch64-darwin",       TARGET_AARCH64, TGT_OS_DARWIN, TGT_FMT_MACHO, 0 },
    { "x86_64-windows-gnu",   TARGET_X86_64,  TGT_OS_WINDOWS, TGT_FMT_COFF, 1 },
    { "x86_64-w64-mingw32",   TARGET_X86_64,  TGT_OS_WINDOWS, TGT_FMT_COFF, 0 },
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
            return 1;
        }
    return 0;
}

const char *target_triple_of(enum target_arch a, enum target_os o)
{
    for (int i = 0; i < g_ntriples; i++)
        if (g_triples[i].canon && g_triples[i].arch == a &&
            g_triples[i].os == o)
            return g_triples[i].name;
    return NULL;
}

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
    return a == TARGET_AARCH64 ? EM_AARCH64 : EM_X86_64;
}

int target_reloc_type(enum target_arch a, enum reloc_kind k)
{
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
    default:          return -1;
    }
}

long target_reloc_addend(enum target_arch a, enum reloc_kind k, long bias)
{
    if (a == TARGET_AARCH64)
        return bias;              /* aarch64 fields are relative to the
                                   * instruction, so no end-of-insn bias */
    switch (k) {
    case RK_CALL:
    case RK_PCREL32:
        return bias - 4;          /* x86-64 rel32 is measured from the END
                                   * of the instruction, four bytes on */
    default:
        return bias;
    }
}
