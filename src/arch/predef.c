/* Which predefined-macro table the selected target uses. Hand-written:
 * the tables themselves are generated, so the choice between them cannot
 * live in either file. */
#include "predef.h"

#include "target.h"
#include "../driver/util.h"

#include <string.h>

static int g_cxx;

void predef_set_cxx(int cxx) { g_cxx = cxx; }
int predef_is_cxx(void) { return g_cxx; }

static const struct predef_macro *arch_table(int *count)
{
    if (g_cxx) {
        switch (target_get()) {
        case TARGET_AARCH64:
            *count = predef_macro_count_cxx_aarch64;
            return predef_macros_cxx_aarch64;
        case TARGET_THUMB:
            /* ARMv8-M Mainline has its own table. Not the v7-M one with
             * __ARM_ARCH patched: the feature macros differ throughout, and
             * a generated file is not hand-parameterised (ARCHITECTURE.md
             * §5). Same arrangement as the two RISC-V widths. */
            if (target_thumb_arch() >= 8) {
                *count = predef_macro_count_cxx_thumbv8m;
                return predef_macros_cxx_thumbv8m;
            }
            *count = predef_macro_count_cxx_thumb;
            return predef_macros_cxx_thumb;
        case TARGET_RISCV32:
            *count = predef_macro_count_cxx_riscv32;
            return predef_macros_cxx_riscv32;
        case TARGET_RISCV64:
            *count = predef_macro_count_cxx_riscv64;
            return predef_macros_cxx_riscv64;
        case TARGET_AVR:
            *count = predef_macro_count_cxx_avr;
            return predef_macros_cxx_avr;
        default:
            *count = predef_macro_count_cxx_x86_64;
            return predef_macros_cxx_x86_64;
        }
    }
    switch (target_get()) {
    case TARGET_AARCH64:
        *count = predef_macro_count_aarch64;
        return predef_macros_aarch64;
    case TARGET_THUMB:
        if (target_thumb_arch() >= 8) {
            *count = predef_macro_count_thumbv8m;
            return predef_macros_thumbv8m;
        }
        *count = predef_macro_count_thumb;
        return predef_macros_thumb;
    case TARGET_RISCV32:
        *count = predef_macro_count_riscv32;
        return predef_macros_riscv32;
    case TARGET_RISCV64:
        *count = predef_macro_count_riscv64;
        return predef_macros_riscv64;
    case TARGET_AVR:
        *count = predef_macro_count_avr;
        return predef_macros_avr;
    default:
        *count = predef_macro_count_x86_64;
        return predef_macros_x86_64;
    }
}

/* What an operating system adds on top of its architecture's table
 * (D-014).
 *
 * Hand-written, where the architecture tables are generated, and the
 * reason is that these are not the reference compiler's opinion about a
 * machine -- they are the NAME of the platform the code will run on, and
 * a system header branches on them to decide which declarations exist.
 * There are few of them and they have been stable for thirty years.
 *
 * Only what the platform's own headers actually read. The unprefixed
 * `linux` and `unix` are left out: gcc defines them only in GNU mode,
 * nothing in a header requires them, and they collide with ordinary
 * identifiers. Nor is __GNUC__ added here, for the reason predef.h
 * already gives -- EmbCC is not gcc, and claiming to be routes headers
 * onto extension paths it cannot honour (THE RULE).
 */
/* EmbLinkOS. Nothing in the OS tree reads a macro today -- the platform
 * is selected by WHICH backend.c gets compiled (Makefile
 * libc-emblinkos) -- so this names it for the first time rather than
 * matching an existing convention. It is still the right thing to
 * define: `x86_64-emblink` is a target in its own right, and code that
 * wants to ask "am I being built for the OS?" should not have to infer
 * it from the absence of __linux__. */
static const struct predef_macro os_emblink[] = {
    { "__emblink__", "1" },
    { "__emblink", "1" },
    { "__EmbLinkOS__", "1" },
};
static const struct predef_macro os_linux[] = {
    { "__linux__", "1" },
    { "__linux", "1" },
    { "__gnu_linux__", "1" },
    { "__unix__", "1" },
    { "__unix", "1" },
};
static const struct predef_macro os_darwin[] = {
    { "__APPLE__", "1" },
    { "__MACH__", "1" },
    { "__unix__", "1" },
    { "__unix", "1" },
};
/* ...and on arm64 the generated aarch64 table (gcc's, AAPCS64) is wrong
 * about the data model target.c's darwin_a64 describes. These replace
 * its entries of the same name, with clang's values for
 * arm64-apple-macos; __CHAR_UNSIGNED__ is dropped (contradicted). */
static const struct predef_macro darwin_a64_model[] = {
    { "__DECIMAL_DIG__", "__LDBL_DECIMAL_DIG__" },
    { "__LDBL_DECIMAL_DIG__", "17" },
    { "__LDBL_DENORM_MIN__", "4.9406564584124654e-324L" },
    { "__LDBL_DIG__", "15" },
    { "__LDBL_EPSILON__", "2.2204460492503131e-16L" },
    { "__LDBL_MANT_DIG__", "53" },
    { "__LDBL_MAX_10_EXP__", "308" },
    { "__LDBL_MAX_EXP__", "1024" },
    { "__LDBL_MAX__", "1.7976931348623157e+308L" },
    { "__LDBL_MIN_10_EXP__", "(-307)" },
    { "__LDBL_MIN_EXP__", "(-1021)" },
    { "__LDBL_MIN__", "2.2250738585072014e-308L" },
    { "__LDBL_NORM_MAX__", "1.7976931348623157e+308L" },
    { "__SIZEOF_LONG_DOUBLE__", "8" },
    { "__WCHAR_MAX__", "2147483647" },
    { "__WCHAR_MIN__", "(-__WCHAR_MAX__ - 1)" },
    { "__WCHAR_TYPE__", "int" },
    { "__WINT_MAX__", "2147483647" },
    { "__WINT_MIN__", "(-__WINT_MAX__ - 1)" },
    { "__WINT_TYPE__", "int" },
};
static const int ndarwin_a64_model =
    (int)(sizeof darwin_a64_model / sizeof *darwin_a64_model);

static int darwin_a64(void)
{
    return target_get() == TARGET_AARCH64 && target_os_get() == TGT_OS_DARWIN;
}

static const struct predef_macro os_windows[] = {
    { "_WIN32", "1" },
    { "_WIN64", "1" },
    { "__MINGW32__", "1" },
    { "__MINGW64__", "1" },
};

/* The FPU on ARMv7E-M and ARMv8-M, when -mfpu= and -mfloat-abi=softfp|hard
 * turn it on. The generated tables are the soft-float ones (see
 * tools/gen-predef.sh), and these are the macros clang changes between that
 * and an FPU build, read off `clang -dM` for both parts and all three ABIs:
 * __SOFTFP__ goes, the VFP version macros and __ARM_FP arrive, and the hard
 * ABI adds __ARM_PCS_VFP.
 *
 * Two deliberate differences from clang, both of them promises this compiler
 * does not make yet. __ARM_FP is 0x4, single precision, where clang says 0x6:
 * bit 1 is hardware half-precision conversion and EmbCC emits no vcvtb. And
 * __ARM_FEATURE_FMA is left out: DSP code reads it to choose fused
 * multiply-add, which EmbCC does not emit. A predefined macro is a promise
 * to the program; the first one this table got wrong, __ARM_FP 0xe on v8-M,
 * compiled lib/rt/softfp.c to nothing. */
static const struct predef_macro thumb_fpu_add[] = {
    { "__ARM_FP", "0x4" },
    { "__ARM_VFPV2__", "1" },
    { "__ARM_VFPV3__", "1" },
    { "__ARM_VFPV4__", "1" },
};
static const struct predef_macro thumb_fpv5_add[] = { { "__ARM_FPV5__", "1" } };
static const struct predef_macro thumb_hard_add[] = { { "__ARM_PCS_VFP", "1" } };

static int thumb_fpu_drops(const char *name)
{
    if (target_get() != TARGET_THUMB || !target_thumb_fpu())
        return 0;
    /* __ARM_PCS names the base calling convention; the hard-float one
     * says __ARM_PCS_VFP INSTEAD, and code tests for either. */
    if (target_thumb_hard_abi() && strcmp(name, "__ARM_PCS") == 0)
        return 1;
    return strcmp(name, "__SOFTFP__") == 0 || strcmp(name, "__ARM_FP") == 0;
}

/* __ELF__ lives in the generated architecture tables, because the
 * compilers they were generated from were the *-elf ones. It is a
 * statement about the OBJECT FORMAT, so on a target that is not ELF it
 * is simply false, and a header that reads it would be told the wrong
 * thing. Dropped rather than overridden: there is no "__ELF__ 0". */
static int contradicted(const char *name)
{
    if (darwin_a64()) {
        if (strcmp(name, "__CHAR_UNSIGNED__") == 0)
            return 1;
        for (int i = 0; i < ndarwin_a64_model; i++)
            if (strcmp(name, darwin_a64_model[i].name) == 0)
                return 1;               /* replaced below */
    }
    return (target_fmt_get() != TGT_FMT_ELF && strcmp(name, "__ELF__") == 0) ||
           thumb_fpu_drops(name);
}

const struct predef_macro *predef_table(int *count)
{
    int narch = 0;
    const struct predef_macro *arch = arch_table(&narch);

    const struct predef_macro *os = NULL;
    int nos = 0;
    switch (target_os_get()) {
    case TGT_OS_EMBLINK:
        os = os_emblink; nos = (int)(sizeof os_emblink / sizeof *os_emblink);
        break;
    case TGT_OS_LINUX:
        os = os_linux;   nos = (int)(sizeof os_linux / sizeof *os_linux);
        break;
    case TGT_OS_DARWIN:
        os = os_darwin;  nos = (int)(sizeof os_darwin / sizeof *os_darwin);
        break;
    case TGT_OS_WINDOWS:
        os = os_windows; nos = (int)(sizeof os_windows / sizeof *os_windows);
        break;
    default:
        break;
    }

    /* The freestanding case is every target that existed before D-014,
     * and it hands back the generated table itself -- no copy, no
     * filtering, nothing to go wrong in the path that everything else
     * depends on. */
    int fpu = target_get() == TARGET_THUMB && target_thumb_fpu();
    if (!os && !fpu && target_fmt_get() == TGT_FMT_ELF) {
        *count = narch;
        return arch;
    }

    static struct predef_macro *merged;
    static int nmerged;
    if (!merged) {
        merged = xmalloc((size_t)(narch + nos + 8 + ndarwin_a64_model) *
                         sizeof *merged);
        for (int i = 0; i < narch; i++)
            if (!contradicted(arch[i].name))
                merged[nmerged++] = arch[i];
        for (int i = 0; i < nos; i++)
            merged[nmerged++] = os[i];
        if (darwin_a64())
            for (int i = 0; i < ndarwin_a64_model; i++)
                merged[nmerged++] = darwin_a64_model[i];
        if (fpu) {
            for (size_t i = 0; i < sizeof thumb_fpu_add / sizeof *thumb_fpu_add; i++)
                merged[nmerged++] = thumb_fpu_add[i];
            if (target_thumb_arch() >= 8)
                merged[nmerged++] = thumb_fpv5_add[0];
            if (target_thumb_hard_abi())
                merged[nmerged++] = thumb_hard_add[0];
        }
    }
    *count = nmerged;
    return merged;
}
