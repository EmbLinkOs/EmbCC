/* LoongArch64's command-line options: GCC's -m options for this target, each
 * accepted when it says what EmbCC emits and refused by name when it
 * asks for something else -- an object built otherwise would link and
 * then disagree with its callers. The driver asks through the backend
 * registry (src/arch/backends.c, `option`); these moved here from the
 * driver's argument loop. */
#include "../backend.h"
#include "../target.h"
#include "../../driver/util.h"

#include <stdlib.h>
#include <string.h>

int loongarch_target_option(const char *arg)
{
    static const char *const exact[] = {
        "-msoft-float", "-msingle-float", "-mdouble-float", "-mrelax",
        "-mno-relax", "-mstrict-align", "-mno-strict-align", "-mlsx",
        "-mno-lsx", "-mlasx", "-mno-lasx", NULL
    };
    static const char *const prefix[] = {
        "-march=", "-mtune=", "-mabi=", "-mfpu=", "-mcmodel=", NULL
    };
    if (!option_listed(arg, exact, prefix))
        return 0;
    /* The flags a LoongArch build passes (clang's and gcc's for
     * loongarch64 bare metal). What EmbCC emits is ONE
     * configuration -- the LA64 base integer ISA, the LP64S
     * soft-float convention, the normal code model -- so each flag
     * either says that (or something it is a valid part of) and is
     * accepted, or asks for something else and is refused by name:
     * an object built for the FPU convention would link and then
     * disagree with every caller about where a double travels. */
    const char *v = strchr(arg, '=');
    v = v ? v + 1 : "";
    if (strncmp(arg, "-march=", 7) == 0) {
        /* the base ISA runs on every one of these */
        static const char *const archs[] = {
            "loongarch64", "la64v1.0", "la64v1.1", "la464", "la664"
        };
        int ok = 0;
        for (unsigned k = 0; k < sizeof archs / sizeof archs[0]; k++)
            ok |= strcmp(v, archs[k]) == 0;
        if (!ok)
            diag_fatal(NULL, 0, "%s is not an LA64 architecture: "
                       "EmbCC emits the LA64 base integer ISA "
                       "(loongarch64, la64v1.0, la64v1.1, la464, "
                       "la664)", arg);
    } else if (strncmp(arg, "-mabi=", 6) == 0) {
        if (strcmp(v, "lp64s") != 0)
            diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                       "the soft-float LP64S convention "
                       "(-mabi=lp64s), which passes floating point "
                       "in the integer registers", arg);
    } else if (strncmp(arg, "-mfpu=", 6) == 0) {
        if (strcmp(v, "none") != 0 && strcmp(v, "0") != 0)
            diag_fatal(NULL, 0, "%s is not supported: EmbCC's "
                       "LoongArch code uses no FPU (-mfpu=none)",
                       arg);
    } else if (strncmp(arg, "-mcmodel=", 9) == 0) {
        /* normal: bl and pcalau12i reach +-128 MiB and +-2 GiB;
         * a medium program fits in that too, and one that does
         * not is refused by the linker, never mislinked */
        if (strcmp(v, "normal") != 0 && strcmp(v, "medium") != 0)
            diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                       "the normal code model (bl, pcalau12i + "
                       "addi.d)", arg);
    } else if (strcmp(arg, "-msingle-float") == 0 ||
               strcmp(arg, "-mdouble-float") == 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                   "soft-float LP64S code (-msoft-float)", arg);
    } else if (strcmp(arg, "-mstrict-align") == 0) {
        diag_fatal(NULL, 0, "-mstrict-align is not supported: "
                   "EmbCC's LoongArch code may access a packed "
                   "member unaligned, as LA64 permits");
    } else if (strcmp(arg, "-mlsx") == 0 ||
               strcmp(arg, "-mlasx") == 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC emits no "
                   "LSX or LASX vector instructions", arg);
    }
    return 1;
}
