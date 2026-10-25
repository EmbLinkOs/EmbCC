/* Which predefined-macro table the selected target uses. Hand-written:
 * the tables themselves are generated, so the choice between them cannot
 * live in either file. */
#include "predef.h"

#include "target.h"
#include "../driver/util.h"

#include <stdio.h>
#include <string.h>

static int g_cxx;

void predef_set_cxx(int cxx) { g_cxx = cxx; }
int predef_is_cxx(void) { return g_cxx; }

/* One generated table, in its C or its C++ flavour. */
#define TABLE(name)                                                        \
    (*count = g_cxx ? predef_macro_count_cxx_##name                        \
                    : predef_macro_count_##name,                           \
     g_cxx ? predef_macros_cxx_##name : predef_macros_##name)

/* Each family's tables under its FAMILY_BUILT (src/arch/target.h): a
 * compiler built without one has no tables for it, and its targets are
 * refused before anything asks. */
static const struct predef_macro *arch_table(int *count)
{
    switch (target_get()) {
#if FAMILY_BUILT(x86)
    case TARGET_X86_64:
        return TABLE(x86_64);
#endif
#if FAMILY_BUILT(aarch64)
    case TARGET_AARCH64:
        return TABLE(aarch64);
#endif
#if FAMILY_BUILT(arm)
    case TARGET_THUMB:
        /* ARMv8-M Mainline has its own table. Not the v7-M one with
         * __ARM_ARCH patched: the feature macros differ throughout, and
         * a generated file is not hand-parameterised (ARCHITECTURE.md
         * §5). Same arrangement as the two RISC-V widths. */
        if (target_thumb_arch() >= 8)
            return TABLE(thumbv8m);
        if (target_arm_a32())
            return TABLE(armv7a);
        if (target_thumb_v8m_base())
            return TABLE(thumbv8mbase);
        if (target_thumb_arch() == 6)
            return TABLE(thumbv6m);
        return TABLE(thumb);
#endif
#if FAMILY_BUILT(riscv)
    case TARGET_RISCV32:
        return TABLE(riscv32);
    case TARGET_RISCV64:
        return TABLE(riscv64);
#endif
#if FAMILY_BUILT(avr)
    case TARGET_AVR:
        return TABLE(avr);
#endif
#if FAMILY_BUILT(mips)
    case TARGET_MIPS32:
        if (target_big_endian())
            return TABLE(mips32eb);
        return TABLE(mips32);
    case TARGET_MIPS64:
        if (target_big_endian())
            return TABLE(mips64eb);
        return TABLE(mips64);
#endif
#if FAMILY_BUILT(loongarch)
    case TARGET_LOONGARCH64:
        return TABLE(loongarch64);
#endif
#if FAMILY_BUILT(tricore)
    case TARGET_TRICORE:
        return TABLE(tricore);
#endif
#if FAMILY_BUILT(xtensa)
    case TARGET_XTENSA:
        return TABLE(xtensa);
#endif
#if FAMILY_BUILT(ppc)
    case TARGET_PPC32:
        return TABLE(ppc32);
#endif
#if FAMILY_BUILT(rx)
    case TARGET_RX:                 /* C++ is refused for RX: one table */
        *count = predef_macro_count_rx;
        return predef_macros_rx;
#endif
#if FAMILY_BUILT(sparc)
    case TARGET_SPARC32:
        return TABLE(sparc32);
#endif
#if FAMILY_BUILT(m68k)
    case TARGET_COLDFIRE:
        return TABLE(coldfire);
#endif
    default:
        internal_error("no predefined macros for %s: its backend is not "
                       "built into this compiler", target_triple_now());
    }
}
#undef TABLE

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
    /* Apple's name for the architecture: the macOS SDK's sys/cdefs.h
     * stops with "Unsupported architecture" without it */
    { "__arm64__", "1" },
    { "__arm64", "1" },
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
 * compiled lib/rt/softfp.c to nothing.
 *
 * The Cortex-M7's FPv5-D16 adds bit 3, double precision, which that unit
 * HAS and which the backend now emits: 0xc where clang says 0xe, for the
 * same half-precision reason. It is also an FPv5 unit, so __ARM_FPV5__ as
 * on the M33. lib/rt/softfp.c reads the bit to keep only the 64-bit
 * integer conversions, which VFP has no instruction for. */
static const struct predef_macro thumb_fpu_add[] = {
    { "__ARM_FP", "0x4" },
    { "__ARM_VFPV2__", "1" },
    { "__ARM_VFPV3__", "1" },
    { "__ARM_VFPV4__", "1" },
};
static const struct predef_macro thumb_fp_dp[] = { { "__ARM_FP", "0xc" } };
static const struct predef_macro thumb_fpv5_add[] = { { "__ARM_FPV5__", "1" } };
static const struct predef_macro thumb_hard_add[] = { { "__ARM_PCS_VFP", "1" } };
/* ARMv7-A's units, as clang defines them for -mfpu=vfpv3[-d16] (0xc:
 * single and double) and vfpv4[-d16] (0xe: half as well, and the fused
 * multiply-add). No FPv5 and no NEON. */
static const struct predef_macro a32_vfp3_add[] = {
    { "__ARM_FP", "0xc" }, { "__ARM_VFPV2__", "1" }, { "__ARM_VFPV3__", "1" },
};
static const struct predef_macro a32_vfp4_add[] = {
    { "__ARM_FP", "0xe" }, { "__ARM_VFPV2__", "1" }, { "__ARM_VFPV3__", "1" },
    { "__ARM_VFPV4__", "1" }, { "__ARM_FEATURE_FMA", "1" },
};

/* -mcmse: the Secure side. ACLE's __ARM_FEATURE_CMSE is a bit set -- 1 the
 * TT instruction (every ARMv8-M table says that much), 2 compiling for the
 * Secure state -- and clang defines 3 under the flag. */
static const struct predef_macro thumb_cmse_add[] = {
    { "__ARM_FEATURE_CMSE", "3" },
};

/* RISC-V's -march= and -mabi= (src/arch/riscv/options.c, riscv_options_done).
 * The generated tables are rv32imac/ilp32 and rv64imac/lp64; these are the
 * macros clang changes from there, read off `clang -dM` for every
 * combination (tests/golden/predef.sh checks each against it): F brings
 * __riscv_f, fdiv, fsqrt, flen and Zicsr, D __riscv_d and flen 64, the
 * hardware-float ABIs replace __riscv_float_abi_soft, and C with F or D
 * the compressed loads and stores of Zcf (RV32 only) and Zcd. Without C
 * the compressed macros go. */
static int riscv_on(void)
{
    return target_get() == TARGET_RISCV32 || target_get() == TARGET_RISCV64;
}

static int riscv_changes(void)
{
    return riscv_on() && (target_riscv_flen() || !target_riscv_rvc() ||
                          target_riscv_zifencei());
}

static int riscv_drops(const char *name)
{
    if (!riscv_on())
        return 0;
    if (target_riscv_abi_flen() && strcmp(name, "__riscv_float_abi_soft") == 0)
        return 1;
    return !target_riscv_rvc() &&
           (strcmp(name, "__riscv_c") == 0 ||
            strcmp(name, "__riscv_compressed") == 0 ||
            strcmp(name, "__riscv_zca") == 0);
}

static int riscv_adds(struct predef_macro *out)
{
    int n = 0, flen = target_riscv_flen(), abi = target_riscv_abi_flen();
    int c = target_riscv_rvc();
    if (flen) {
        out[n].name = "__riscv_f";      out[n++].value = "2002000";
        out[n].name = "__riscv_fdiv";   out[n++].value = "1";
        out[n].name = "__riscv_fsqrt";  out[n++].value = "1";
        out[n].name = "__riscv_flen";   out[n++].value = flen == 64 ? "64" : "32";
        out[n].name = "__riscv_zicsr";  out[n++].value = "2000000";
    }
    if (flen == 64) {
        out[n].name = "__riscv_d";      out[n++].value = "2002000";
    }
    if (abi) {
        out[n].name = abi == 64 ? "__riscv_float_abi_double"
                                : "__riscv_float_abi_single";
        out[n++].value = "1";
    }
    if (c && flen && target_get() == TARGET_RISCV32) {
        out[n].name = "__riscv_zcf";    out[n++].value = "1000000";
    }
    if (c && flen == 64) {
        out[n].name = "__riscv_zcd";    out[n++].value = "1000000";
    }
    if (target_riscv_zifencei()) {
        out[n].name = "__riscv_zifencei"; out[n++].value = "2000000";
    }
    return n;
}

/* The DSP extension: ARMv7E-M (Cortex-M4/M7), and ARMv8-M Mainline with
 * it (Cortex-M33, -march=armv8-m.main+dsp). The generated tables are
 * clang's thumbv7m and thumbv8m.main, neither of which has it; these are
 * what clang adds for -mcpu=cortex-m4 or cortex-m33, read off its -dM
 * (tests/golden/predef.sh compares every Cortex-M part against it). On
 * v7E-M the architecture macro is a different NAME, so __ARM_ARCH_7M__
 * goes: CMSIS's cmsis_gcc.h selects __SSAT and the DSP intrinsics on
 * __ARM_ARCH_7EM__ and __ARM_FEATURE_DSP, and without them a CMSIS-DSP
 * build took its plain-C fallbacks or did not build.
 *
 * __ARM_FEATURE_FMA, which clang also adds, is left out for the reason
 * thumb_fpu_add gives. ARMv7-A (A32) has its own table, already with them. */
static const struct predef_macro thumb_dsp_add[] = {
    { "__ARM_FEATURE_DSP", "1" },
    { "__ARM_FEATURE_SIMD32", "1" },
};
static const struct predef_macro thumb_v7em_add[] = {
    { "__ARM_ARCH_7EM__", "1" },
};

static int thumb_dsp(void)
{
    return target_get() == TARGET_THUMB && !target_arm_a32() &&
           target_thumb_em() &&
           (target_thumb_arch() == 7 || target_thumb_arch() >= 8);
}

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

/* AVR's -mmcu= (src/arch/avr/options.c). The generated table is clang's
 * for -mmcu=atmega328p; for another part of the same avr5 core clang's
 * answer differs in one macro, the part's own name (__AVR_ATmega168__),
 * so that one is replaced and the table is otherwise the part's. */
static int avr_other_part(void)
{
#if FAMILY_BUILT(avr)
    const struct avr_mcu *m = target_avr_mcu();
    return m && strcmp(m->macro, "__AVR_ATmega328P__") != 0;
#else
    return 0;                       /* no AVR in this compiler */
#endif
}

/* A PIC32 part (--mcu= on mipsel, src/arch/mips/options.c): the macros
 * XC32 defines for it, which the device pack's headers test -- the part's
 * own (__32MZ2048EFH100__ picks its header in <xc.h>), the family's, XC32's
 * name and version, and __LANGUAGE_C__, which tells C from assembly there
 * (the driver swaps it for __LANGUAGE_ASSEMBLY__ in a .S file). The
 * pack's header defines the part's features (__PIC32_HAS_FPU64, ...). */
static int pic32_adds(struct predef_macro *m)
{
    static char part_u[48], part[48], fam_u[16], fam[16];
    const char *p = target_pic32_part();
    int n = 0;
    snprintf(part_u, sizeof part_u, "__%s__", p);
    snprintf(part, sizeof part, "__%s", p);
    /* the family is the two letters after 32: MX, MZ, MK, MM */
    snprintf(fam_u, sizeof fam_u, "__PIC32%.2s__", p + 2);
    snprintf(fam, sizeof fam, "__PIC32%.2s", p + 2);
    m[n].name = part_u; m[n++].value = "1";
    m[n].name = part;   m[n++].value = "1";
    m[n].name = fam_u;  m[n++].value = "1";
    m[n].name = fam;    m[n++].value = "1";
    m[n].name = "__PIC32M"; m[n++].value = "1";
    m[n].name = "__PIC32M__"; m[n++].value = "1";
    m[n].name = "__XC32";   m[n++].value = "1";
    m[n].name = "__XC32__"; m[n++].value = "1";
    m[n].name = "__XC";     m[n++].value = "1";
    m[n].name = "__XC__";   m[n++].value = "1";
    /* the release whose behaviour EmbCC's <pic32m_builtins.h> follows */
    m[n].name = "__XC32_VERSION"; m[n++].value = "4600";
    m[n].name = "__XC32_VERSION__"; m[n++].value = "4600";
    m[n].name = "__LANGUAGE_C__"; m[n++].value = "1";
    m[n].name = "__LANGUAGE_C";   m[n++].value = "1";
    return n;
}

/* __ELF__ lives in the generated architecture tables, because the
 * compilers they were generated from were the *-elf ones. It is a
 * statement about the OBJECT FORMAT, so on a target that is not ELF it
 * is simply false, and a header that reads it would be told the wrong
 * thing. Dropped rather than overridden: there is no "__ELF__ 0". */
/* _Float16 is EmbCC's on every target (src/ir/irgen.c, lib/rt/half.c),
 * and its float.h-style description with it: the generated tables of
 * the targets whose reference compiler has the type carry these
 * already, and the others get them here. Clang's values. */
static const struct predef_macro flt16_add[] = {
    { "__FLT16_DECIMAL_DIG__", "5" },
    { "__FLT16_DENORM_MIN__", "5.9604644775390625e-8F16" },
    { "__FLT16_DIG__", "3" },
    { "__FLT16_EPSILON__", "9.765625e-4F16" },
    { "__FLT16_HAS_DENORM__", "1" },
    { "__FLT16_HAS_INFINITY__", "1" },
    { "__FLT16_HAS_QUIET_NAN__", "1" },
    { "__FLT16_MANT_DIG__", "11" },
    { "__FLT16_MAX_10_EXP__", "4" },
    { "__FLT16_MAX_EXP__", "16" },
    { "__FLT16_MAX__", "6.5504e+4F16" },
    { "__FLT16_MIN_10_EXP__", "(-4)" },
    { "__FLT16_MIN_EXP__", "(-13)" },
    { "__FLT16_MIN__", "6.103515625e-5F16" },
    { "__FLT16_NORM_MAX__", "6.5504e+4F16" },
};

static int table_has(const struct predef_macro *t, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].name, name))
            return 1;
    return 0;
}

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
           thumb_fpu_drops(name) || riscv_drops(name) ||
           (thumb_dsp() && target_thumb_arch() == 7 &&
            strcmp(name, "__ARM_ARCH_7M__") == 0) ||
           (target_thumb_cmse() && strcmp(name, "__ARM_FEATURE_CMSE") == 0) ||
           (avr_other_part() && strcmp(name, "__AVR_ATmega328P__") == 0);
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
    int cmse = target_thumb_cmse();
    int rv = riscv_changes();
    int dsp = thumb_dsp();
    int avr = avr_other_part();
    int pic32 = target_pic32_part() != NULL;
    /* (C only: the C++ front end has no _Float16, and libstdc++ declares
     * one where __FLT16_MANT_DIG__ says it may) */
    int f16 = !predef_is_cxx() && !table_has(arch, narch, "__FLT16_MANT_DIG__");
    if (!os && !fpu && !cmse && !rv && !dsp && !avr && !pic32 && !f16 &&
        target_fmt_get() == TGT_FMT_ELF) {
        *count = narch;
        return arch;
    }

    static struct predef_macro *merged;
    static int nmerged;
    if (!merged) {
        merged = xmalloc((size_t)(narch + nos + 40 + ndarwin_a64_model +
                                  (int)(sizeof flt16_add / sizeof *flt16_add)) *
                         sizeof *merged);
        for (int i = 0; i < narch; i++)
            if (!contradicted(arch[i].name))
                merged[nmerged++] = arch[i];
        for (int i = 0; i < nos; i++)
            merged[nmerged++] = os[i];
        if (darwin_a64())
            for (int i = 0; i < ndarwin_a64_model; i++)
                merged[nmerged++] = darwin_a64_model[i];
        if (cmse)
            merged[nmerged++] = thumb_cmse_add[0];
        if (dsp && target_thumb_arch() == 7)
            merged[nmerged++] = thumb_v7em_add[0];
        if (dsp)
            for (size_t i = 0; i < sizeof thumb_dsp_add / sizeof *thumb_dsp_add; i++)
                merged[nmerged++] = thumb_dsp_add[i];
        if (rv)
            nmerged += riscv_adds(merged + nmerged);
#if FAMILY_BUILT(avr)
        if (avr) {
            merged[nmerged].name = target_avr_mcu()->macro;
            merged[nmerged++].value = "1";
        }
#endif
        if (pic32)
            nmerged += pic32_adds(merged + nmerged);
        if (f16)
            for (size_t i = 0; i < sizeof flt16_add / sizeof *flt16_add; i++)
                merged[nmerged++] = flt16_add[i];
        if (fpu && target_arm_a32()) {
            int v4 = target_arm_vfp(NULL) == 4;
            const struct predef_macro *add = v4 ? a32_vfp4_add : a32_vfp3_add;
            int nadd = v4 ? (int)(sizeof a32_vfp4_add / sizeof *a32_vfp4_add)
                          : (int)(sizeof a32_vfp3_add / sizeof *a32_vfp3_add);
            for (int i = 0; i < nadd; i++)
                merged[nmerged++] = add[i];
            if (target_thumb_hard_abi())
                merged[nmerged++] = thumb_hard_add[0];
        } else if (fpu) {
            for (size_t i = 0; i < sizeof thumb_fpu_add / sizeof *thumb_fpu_add; i++) {
                if (i == 0 && target_thumb_fpu_dp())
                    merged[nmerged++] = thumb_fp_dp[0];
                else
                    merged[nmerged++] = thumb_fpu_add[i];
            }
            if (target_thumb_arch() >= 8 || target_thumb_fpu_dp())
                merged[nmerged++] = thumb_fpv5_add[0];
            if (target_thumb_hard_abi())
                merged[nmerged++] = thumb_hard_add[0];
        }
    }
    *count = nmerged;
    return merged;
}
