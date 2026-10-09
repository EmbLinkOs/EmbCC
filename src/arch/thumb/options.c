/* ARM's command-line options: the -m options every Cortex-M and Cortex-A
 * build passes (-mcpu=, -march=, -mtune=, -mfpu=, -mfloat-abi=, -mabi=,
 * -mthumb, -marm, -mcmse and the rest), each accepted when it says what
 * EmbCC emits and refused by name when it asks for something else. They
 * are recorded while the arguments are read and settled together once
 * all are (thumb_options_done): -mfpu= and -mcpu= may come in either
 * order. The driver asks through the backend registry
 * (src/arch/backends.c, `option` and `options_done`); this moved here
 * from the driver's argument loop. */
#include "../backend.h"
#include "../target.h"
#include "../../driver/util.h"

#include <stdlib.h>
#include <string.h>

/* -mfpu=, -mfloat-abi= and -mcpu=, as recorded while parsing. */
static const char *g_arm_fpu;
static const char *g_arm_float_abi;
static const char *g_arm_cpu;
static int g_arm_cmse;             /* -mcmse was given */
/* The unit -march='s +fp, +fp.dp or +nofp names: the FPU when -mfpu= is
 * not given (or is auto), as GCC takes the pair. */
static const char *g_arm_march_fpu;

/* What the two ARM float flags mean together, decided once every argument has
 * been seen.
 *
 * The FPUs EmbCC knows are the units its parts carry: FPv4-SP-D16 on a
 * Cortex-M4F and FPv5-D16 on a Cortex-M7 (both ARMv7E-M), and FPv5-SP-D16
 * on a Cortex-M33 (ARMv8-M Mainline). The first and the last are single
 * precision, so `double` stays in software there; FPv5-D16 computes
 * `double` too, and the backend emits .f64 arithmetic for it. Anything
 * else is refused BY NAME -- an unknown name is not a thing to guess at.
 *
 * The part's own unit, which an -eabihf triple implies, follows -mcpu=:
 * thumbv7em-none-eabihf alone is a Cortex-M4F, as it is to clang, and
 * with -mcpu=cortex-m7 it is the M7 and its double-precision unit.
 *
 *   soft (the default, as for arm-none-eabi-gcc): no FPU instructions, even
 *       with an -mfpu= -- which is GCC's reading of the pair.
 *   softfp: FPU instructions, float arguments in the CORE registers. Links
 *       with soft-float objects, because the calling convention is theirs.
 *   hard: FPU instructions, and floating point passed and returned in
 *       s0-s15 / d0-d7 (AAPCS-VFP); docs/manual/invoking.md, -mfloat-abi=.
 *
 * The object says which it was built for (Tag_FP_arch, Tag_ABI_HardFP_use,
 * Tag_ABI_VFP_args) and the predefined macros say so to the program
 * (__ARM_FP, __ARM_VFPV4__, __SOFTFP__); both follow from what is set here. */
void thumb_options_done(void)
{
    /* -mcmse needs the security extension, which only ARMv8-M has. */
    if (g_arm_cmse && !target_thumb_v8m())
        diag_fatal(NULL, 0, "-mcmse is the Secure side of ARMv8-M's "
                   "security extension, and %s is not ARMv8-M: use "
                   "thumbv8m.main-none-eabi or thumbv8m.base-none-eabi",
                   target_triple_now());
    /* ARMv7-A: VFPv3 or VFPv4, D16 or D32 -- every Cortex-A's unit but
     * NEON's SIMD, which nothing here emits and so nothing may claim. The
     * code is the Cortex-M7's (single and double precision on d0-d15),
     * each instruction under a condition field. armv7a-none-eabihf is
     * -mfpu=vfpv3-d16 -mfloat-abi=hard: the unit every Cortex-A with an
     * FPU has. */
    if (target_arm_a32()) {
        static const struct { const char *name; int ver, d32; } units[] = {
            { "vfpv3-d16", 3, 0 }, { "vfpv3", 3, 1 }, { "vfp3", 3, 1 },
            { "vfpv4-d16", 4, 0 }, { "vfpv4", 4, 1 }, { "vfp4", 4, 1 },
            { NULL, 0, 0 }
        };
        int hfa = target_thumb_hf_name(), unit = -1;
        const char *a = g_arm_float_abi ? g_arm_float_abi : hfa ? "hard" : "soft";
        const char *u = g_arm_fpu ? g_arm_fpu : hfa ? "vfpv3-d16" : NULL;
        int named = u && strcmp(u, "none") && strcmp(u, "soft") &&
                    strcmp(u, "auto");
        if (strcmp(a, "soft") && strcmp(a, "softfp") && strcmp(a, "hard"))
            diag_fatal(NULL, 0, "-mfloat-abi=%s is not an ARM float ABI: it "
                       "is one of soft, softfp and hard", a);
        for (int k = 0; named && units[k].name; k++)
            if (!strcmp(u, units[k].name))
                unit = k;
        if (named && unit < 0)
            diag_fatal(NULL, 0, "-mfpu=%s is not supported on %s: EmbCC "
                       "emits VFPv3 or VFPv4 (-mfpu=vfpv3-d16, vfpv3, "
                       "vfpv4-d16, vfpv4) there, and no NEON (Advanced SIMD) "
                       "instruction", u, target_triple_now());
        if (!strcmp(a, "soft"))
            return;                    /* no FPU instructions, as GCC reads it */
        if (!named)
            diag_fatal(NULL, 0, "-mfloat-abi=%s needs an FPU to use: add "
                       "-mfpu=vfpv3-d16 (or vfpv3, vfpv4-d16, vfpv4)", a);
        target_set_thumb_hard_abi(!strcmp(a, "hard"));
        target_set_thumb_fpu(1);
        target_set_thumb_fpu_dp(1);
        target_set_arm_vfp(units[unit].ver, units[unit].d32);
        return;
    }
    /* An -eabihf triple is shorthand for the part's FPU and the hard
     * convention; a flag that says otherwise wins, as with clang. */
    int hf = target_thumb_hf_name();
    int m7 = g_arm_cpu && strcmp(g_arm_cpu, "cortex-m7") == 0;
    const char *hf_fpu = target_thumb_arch() >= 8 ? "fpv5-sp-d16"
                       : m7 ? "fpv5-d16" : "fpv4-sp-d16";
    const char *abi = g_arm_float_abi ? g_arm_float_abi : hf ? "hard" : "soft";
    if ((!g_arm_fpu || !strcmp(g_arm_fpu, "auto")) && g_arm_march_fpu)
        g_arm_fpu = g_arm_march_fpu;
    if (!g_arm_fpu && hf)
        g_arm_fpu = hf_fpu;
    int fpu_named = g_arm_fpu && strcmp(g_arm_fpu, "none") != 0 &&
                    strcmp(g_arm_fpu, "soft") != 0 && strcmp(g_arm_fpu, "auto") != 0;
    if (!g_arm_fpu && !g_arm_float_abi)
        return;
    if (strcmp(abi, "soft") && strcmp(abi, "softfp") && strcmp(abi, "hard"))
        diag_fatal(NULL, 0, "-mfloat-abi=%s is not an ARM float ABI: it is "
                   "one of soft, softfp and hard", abi);
    /* No ARMv6-M part has an FPU: only the base standard means anything. */
    if (target_thumb_arch() == 6 && (fpu_named || strcmp(abi, "soft")))
        diag_fatal(NULL, 0, "%s%s on %s: %s has no FPU, so floating point "
                   "is soft and travels in the core registers",
                   fpu_named ? "-mfpu=" : "-mfloat-abi=",
                   fpu_named ? g_arm_fpu : abi, target_triple_now(),
                   target_thumb_v8m_base()
                   ? "an ARMv8-M Baseline core (Cortex-M23)"
                   : "an ARMv6-M core (Cortex-M0, M0+, M1)");
    if (fpu_named) {
        int v8 = target_thumb_arch() >= 8;
        int dp = strcmp(g_arm_fpu, "fpv5-d16") == 0;
        /* FPv5-D16 on ARMv8-M is refused with the others: the Mainline
         * part this backend knows, the Cortex-M33, has the single-precision
         * FPv5, and its attributes and tables are the only ones checked. */
        if (v8 ? strcmp(g_arm_fpu, "fpv5-sp-d16") != 0
               : strcmp(g_arm_fpu, "fpv4-sp-d16") != 0 && !dp)
            diag_fatal(NULL, 0, "-mfpu=%s is not supported on %s: EmbCC "
                       "emits VFP for %s and nothing else: another unit's "
                       "instruction set and attributes are unchecked here",
                       g_arm_fpu, target_triple_now(),
                       v8 ? "the Cortex-M33's unit (-mfpu=fpv5-sp-d16)"
                          : "the Cortex-M4F's unit (-mfpu=fpv4-sp-d16) and "
                            "the Cortex-M7's (-mfpu=fpv5-d16)");
        if (!v8 && !target_thumb_em())
            diag_fatal(NULL, 0, "-mfpu=%s is an ARMv7E-M unit, and the part "
                       "is ARMv7-M (a Cortex-M3 has no FPU); add -mcpu=%s",
                       g_arm_fpu, dp ? "cortex-m7" : "cortex-m4");
        /* The double-precision unit is the M7's and no other part's: an
         * M4 told it has one would run .f64 instructions it does not
         * implement, which is a UsageFault at the first double. */
        if (dp && g_arm_cpu && !m7)
            diag_fatal(NULL, 0, "-mfpu=fpv5-d16 is the Cortex-M7's "
                       "double-precision unit, and -mcpu=%s does not have "
                       "it; the Cortex-M4F's is -mfpu=fpv4-sp-d16", g_arm_cpu);
    }
    if (!strcmp(abi, "soft"))
        return;                        /* no FPU instructions, as GCC reads it */
    if (!fpu_named)
        diag_fatal(NULL, 0, "-mfloat-abi=%s needs an FPU to use: add "
                   "-mfpu=fpv4-sp-d16 (Cortex-M4F), -mfpu=fpv5-d16 "
                   "(Cortex-M7) or -mfpu=fpv5-sp-d16 (Cortex-M33)", abi);
    /* hard: the FPU's arithmetic, and floating point passed and
     * returned in s0-s15 / d0-d7 (AAPCS-VFP). The runtime helpers keep the
     * base convention either way, as the RTABI requires. The convention
     * is the same for every unit: a double travels in a d register on an
     * M4F too, which only cannot compute with it. */
    target_set_thumb_hard_abi(!strcmp(abi, "hard"));
    target_set_thumb_fpu(1);
    target_set_thumb_fpu_dp(strcmp(g_arm_fpu, "fpv5-d16") == 0);
    /* -mcmse with the FPU in use: an entry function would have to clear
     * s0-s15 and FPSCR when the Secure state's FP context is active
     * (CONTROL_S.SFPA), and a call to the Non-secure state would have to
     * hand the hard-float convention's arguments over in VFP registers.
     * Neither is emitted, so the combination is refused rather than
     * leaking a Secure float into the Non-secure state. */
    if (g_arm_cmse && target_thumb_fpu())
        diag_fatal(NULL, 0, "-mcmse with an FPU (-mfpu=, -mfloat-abi=softfp "
                   "or hard, or an -eabihf triple) is not supported: EmbCC "
                   "does not clear the floating-point registers a "
                   "cmse_nonsecure_entry function must clear; build the "
                   "Secure side with -mfloat-abi=soft");
}

int thumb_target_option(const char *arg)
{
    if (strcmp(arg, "-mcmse") == 0) {
        /* The Secure side of ARMv8-M's security extension (ACLE's
         * CMSE): cmse_nonsecure_entry and cmse_nonsecure_call, and
         * __ARM_FEATURE_CMSE 3. Checked against the architecture once
         * every argument is read: -mcpu= may come after it. */
        g_arm_cmse = 1;
        target_set_thumb_cmse(1);
        return 1;
    }
    static const char *const exact[] = {
        "-mthumb", "-marm", "-mthumb-interwork", "-mno-thumb-interwork",
        "-mslow-flash-data", "-munaligned-access", "-mno-unaligned-access",
        NULL
    };
    static const char *const prefix[] = {
        "-mcpu=", "-mfpu=", "-mfloat-abi=", "-march=", "-mtune=", "-mabi=",
        NULL
    };
    if (option_listed(arg, exact, prefix)) {
        /* The ARM machine flags every Cortex-M build passes. They
         * were "unknown argument" before, which stops a kernel's
         * existing Makefile dead -- and the two that describe the
         * FLOAT ABI are the ones that must not be guessed at,
         * because getting them wrong is an ABI mismatch the linker
         * cannot see (EmbCC emits no .ARM.attributes yet either).
         *
         * -mcpu= selects the sub-architecture, which EmbCC now
         * carries. -mthumb is the only state this backend has, so it
         * is a no-op that has to be accepted. -marm asks for the ARM
         * instruction set, which a Cortex-M does not have at all. */
        const char *v = strchr(arg, '=');
        v = v ? v + 1 : NULL;
        /* ARMv7-A in ARM state: -marm is what is emitted, -mthumb
         * would be Thumb-2 on a Cortex-A, which EmbCC does not emit
         * there (the Thumb-2 it emits is the Cortex-M levels'). The
         * rest of the flags are read below as on the Cortex-M levels,
         * with the parts and units this target has. */
        if (target_arm_a32()) {
            if (strcmp(arg, "-marm") == 0)
                return 1;
            /* -march=armv7-a (with GCC's +ext spellings, whose units
             * -mfpu= and thumb_options_done decide) and -mtune= for a
             * Cortex-A: scheduling, which EmbCC does not tune */
            if (strncmp(arg, "-march=", 7) == 0) {
                if (strncmp(v, "armv7-a", 7) != 0 &&
                    strncmp(v, "armv7ve", 7) != 0)
                    diag_fatal(NULL, 0, "-march=%s is not supported on "
                               "%s: EmbCC emits ARMv7-A code there "
                               "(-march=armv7-a)", v, target_triple_now());
                return 1;
            }
            if (strncmp(arg, "-mtune=", 7) == 0) {
                if (strncmp(v, "cortex-a", 8) != 0 &&
                    strcmp(v, "generic-armv7-a") != 0)
                    diag_fatal(NULL, 0, "-mtune=%s is not a Cortex-A core",
                               v);
                return 1;
            }
            if (strcmp(arg, "-mthumb") == 0)
                diag_fatal(NULL, 0, "-mthumb is not supported on %s: "
                           "EmbCC emits ARM (A32) code for a Cortex-A; "
                           "Thumb-2 is for the Cortex-M targets "
                           "(thumbv7m-none-eabi and the others)",
                           target_triple_now());
            if (strncmp(arg, "-mcpu=", 6) == 0) {
                /* The ARMv7-A cores. All of them run what is emitted:
                 * no divide instruction is used (the A7, A12, A15 and
                 * A17 have one; the code calls __aeabi_idiv anyway),
                 * and no VFP or NEON. */
                static const char *const a7cores[] = {
                    "cortex-a5", "cortex-a7", "cortex-a8", "cortex-a9",
                    "cortex-a12", "cortex-a15", "cortex-a17", "generic",
                    NULL
                };
                int known = 0;
                for (int k = 0; a7cores[k]; k++)
                    known |= strcmp(v, a7cores[k]) == 0;
                if (!known)
                    diag_fatal(NULL, 0, "-mcpu=%s is not supported on %s: "
                               "EmbCC emits ARMv7-A (cortex-a5, a7, a8, "
                               "a9, a12, a15, a17) here; a Cortex-M is "
                               "one of the thumb targets, and an ARMv7-R "
                               "core's profile is not this one", v,
                               target_triple_now());
                g_arm_cpu = v;
                return 1;
            }
        }
        if (strcmp(arg, "-marm") == 0)
            diag_fatal(NULL, 0, "-marm is not supported: a Cortex-M "
                       "has no ARM instruction set, only Thumb");
        if (strcmp(arg, "-mthumb") == 0)
            return 1;          /* the only state there is */
        /* Interworking is between ARM and Thumb code, and a
         * Cortex-M runs only Thumb: every call and return here is
         * already one bx/blx would make (bit 0 set), so both
         * spellings describe what is emitted. */
        if (strcmp(arg, "-mthumb-interwork") == 0 ||
            strcmp(arg, "-mno-thumb-interwork") == 0)
            return 1;
        /* A hint: keep constants out of literal pools in slow flash.
         * The compiled code has none -- constants and addresses are
         * movw/movt -- so there is nothing to move. */
        if (strcmp(arg, "-mslow-flash-data") == 0)
            return 1;
        /* The procedure call standard. EmbCC's is AAPCS (the base
         * standard, or AAPCS-VFP under -mfloat-abi=hard); aapcs-linux
         * is the same convention with int-sized enums, which is what
         * EmbCC's enums are. The pre-EABI conventions pass and lay
         * out differently, and an object built for one links and
         * then disagrees with its callers. */
        if (strncmp(arg, "-mabi=", 6) == 0) {
            if (strcmp(v, "aapcs") && strcmp(v, "aapcs-linux"))
                diag_fatal(NULL, 0, "-mabi=%s is not supported: EmbCC "
                           "emits the AAPCS (-mabi=aapcs, or "
                           "aapcs-linux, whose int-sized enums are "
                           "EmbCC's too); %s passes arguments and lays "
                           "out data differently", v, v);
            return 1;
        }
        /* ARMv7-M and ARMv8-M Mainline load and store a word or a
         * halfword at any address (LDR/STR/LDRH/STRH; never
         * LDRD/STRD/LDM/STM, which this backend keeps to aligned
         * addresses). -munaligned-access says so, and is what is
         * emitted. */
        if (strcmp(arg, "-munaligned-access") == 0)
            return 1;
        /* -mno-unaligned-access is a promise this backend does not
         * keep: a packed struct's int member is one ldr.w at its
         * odd address, and a struct whose alignment is below four
         * (a packed one, or `struct { char c[5]; }`) is copied, and
         * passed by value, a word at a time from wherever it is.
         * That is ARMv7-M's default and it is fine there; under the
         * flag the same image faults wherever unaligned accesses
         * trap (CCR.UNALIGN_TRP, or Device memory on an M7). */
        if (strcmp(arg, "-mno-unaligned-access") == 0)
            diag_fatal(NULL, 0, "-mno-unaligned-access is not "
                       "supported: EmbCC's ARMv7-M code uses word and "
                       "halfword loads and stores at unaligned addresses "
                       "(packed struct members; copies and by-value "
                       "passing of structs aligned below 4), which the "
                       "architecture allows and this flag forbids");
        /* -mtune= picks a core to schedule for, which EmbCC does
         * not do: any Cortex-M part is accepted, and changes nothing. */
        if (strncmp(arg, "-mtune=", 7) == 0) {
            static const char *const parts[] = {
                "cortex-m0", "cortex-m0plus", "cortex-m1", "cortex-m3",
                "cortex-m4", "cortex-m7", "cortex-m23", "cortex-m33",
                "cortex-m35p", "cortex-m55", "cortex-m85",
                "generic-armv7-m", "generic-armv7e-m", NULL
            };
            int ok = 0;
            for (int k = 0; parts[k]; k++)
                ok |= !strcmp(v, parts[k]);
            if (!ok)
                diag_fatal(NULL, 0, "-mtune=%s is not a Cortex-M core", v);
            return 1;
        }
        /* -march= selects the level as -mcpu= does, with GCC's
         * extension spellings: +fp, +fp.dp and +nofp name the unit
         * (which -mfpu= overrides), +dsp and +nodsp the DSP set on
         * ARMv8-M Mainline. */
        if (strncmp(arg, "-march=", 7) == 0) {
            const char *plus = strchr(v, '+');
            size_t bl = plus ? (size_t)(plus - v) : strlen(v);
            int arch, em = 0, base8 = 0;
            if ((bl == 7 && !strncmp(v, "armv6-m", 7)) ||
                (bl == 8 && !strncmp(v, "armv6s-m", 8)))
                arch = 6;
            else if (bl == 7 && !strncmp(v, "armv7-m", 7))
                arch = 7;
            else if (bl == 8 && !strncmp(v, "armv7e-m", 8))
                arch = 7, em = 1;
            else if (bl == 12 && !strncmp(v, "armv8-m.base", 12))
                arch = 6, base8 = 1;
            else if (bl == 12 && !strncmp(v, "armv8-m.main", 12))
                arch = 8;
            else
                diag_fatal(NULL, 0, "-march=%.*s is not an architecture "
                           "EmbCC emits for a Cortex-M: armv6-m, "
                           "armv6s-m, armv7-m, armv7e-m, armv8-m.base, "
                           "armv8-m.main", (int)bl, v);
            for (const char *x = plus; x && *x; ) {
                const char *e = strchr(x + 1, '+');
                size_t n = e ? (size_t)(e - x) : strlen(x);
                if (n == 3 && !strncmp(x, "+fp", 3) && arch >= 7)
                    g_arm_march_fpu = arch == 8 ? "fpv5-sp-d16"
                                                : "fpv4-sp-d16";
                else if (n == 6 && !strncmp(x, "+fp.dp", 6) && arch >= 7)
                    g_arm_march_fpu = "fpv5-d16";
                else if (n == 5 && !strncmp(x, "+nofp", 5))
                    g_arm_march_fpu = "none";
                else if (n == 4 && !strncmp(x, "+dsp", 4) && arch == 8)
                    em = 1;
                else if (n == 6 && !strncmp(x, "+nodsp", 6) && arch == 8)
                    em = 0;
                else
                    diag_fatal(NULL, 0, "-march=%s: the extension '%.*s' "
                               "is not one EmbCC emits for that "
                               "architecture (+fp, +fp.dp, +nofp, and +dsp "
                               "or +nodsp on armv8-m.main)", v, (int)n, x);
                x = e;
            }
            if (base8) {
                target_set_thumb_v8m_base();
            } else {
                target_set_thumb_arch(arch);
                target_set_thumb_em(em);
            }
            return 1;
        }
        if (strncmp(arg, "-mcpu=", 6) == 0) {
            /* Only the parts whose ISA this backend really emits.
             * An F part is refused by name rather than accepted and
             * built soft-float: its ABI passes floats in s0-s15 and
             * an object built the other way links and then reads its
             * arguments from the wrong registers. */
            /* The ARMv6-M parts select that level: Thumb-1, which the
             * backend emits for them (src/arch/thumb/v6m.c). They used
             * to be taken as ARMv7-M, and the code that came out used
             * ldr.w and IT blocks -- a HardFault at the first one.
             *
             * ARMv8-M Baseline (Cortex-M23) is that selection with
             * the divides and the exclusives turned on (v6m.c), on
             * any Thumb triple, as clang takes it. */
            if (!strcmp(v, "cortex-m23")) {
                target_set_thumb_v8m_base();
                g_arm_cpu = v;
                return 1;
            }
            if (!strcmp(v, "cortex-m0") || !strcmp(v, "cortex-m0plus") ||
                !strcmp(v, "cortex-m1")) {
                target_set_thumb_arch(6);
                target_set_thumb_em(0);
                g_arm_cpu = v;
                return 1;
            }
            /* The part has one architecture, whatever the triple
             * said, as clang takes it: -mcpu=cortex-m33 on a thumbv7em
             * name is ARMv8-M Mainline (__ARM_ARCH 8), and cortex-m4 on
             * a thumbv8m.main one is ARMv7E-M. It used to raise only an
             * ARMv6-M level, and the first case built v7-M code and
             * macros for an M33. (ARM state's triple keeps its level:
             * the A-profile encoder is not chosen by a Cortex-M name.) */
            if (!target_arm_a32() &&
                (!strcmp(v, "cortex-m3") || !strcmp(v, "cortex-m4") ||
                 !strcmp(v, "cortex-m7")))
                target_set_thumb_arch(7);
            if (!target_arm_a32() && !strcmp(v, "cortex-m33"))
                target_set_thumb_arch(8);
            if (!strcmp(v, "cortex-m3"))
                target_set_thumb_em(0);
            else if (!strcmp(v, "cortex-m4") || !strcmp(v, "cortex-m7") ||
                     !strcmp(v, "cortex-m33"))
                target_set_thumb_em(1);
            else
                diag_fatal(NULL, 0, "-mcpu=%s is not a part EmbCC knows: "
                           "it emits ARMv6-M (cortex-m0, m0plus, m1), "
                           "ARMv8-M Baseline (cortex-m23), ARMv7-M and "
                           "ARMv7E-M (cortex-m3, m4, m7) and ARMv8-M "
                           "Mainline (cortex-m33)", v);
            /* Kept for thumb_options_done: which unit the part's
             * -eabihf name implies depends on which part it is. */
            g_arm_cpu = v;
            return 1;
        }
        /* The FPU and the float ABI are RECORDED here and resolved
         * after every argument has been read (see thumb_options_done):
         * `-mfloat-abi=softfp -mfpu=fpv4-sp-d16` and the other order
         * mean the same thing, and neither flag decides anything alone. */
        if (strncmp(arg, "-mfpu=", 6) == 0) {
            g_arm_fpu = v;
            return 1;
        }
        if (strncmp(arg, "-mfloat-abi=", 12) == 0) {
            g_arm_float_abi = v;
            return 1;
        }
        return 1;
    }
    return 0;
}
