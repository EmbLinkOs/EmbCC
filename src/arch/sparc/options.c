/* SPARC's command-line options: GCC's -m options for this target, each
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

int sparc_target_option(const char *arg)
{
    static const char *const exact[] = {
        "-msoft-float", "-mhard-float", "-mfpu", "-mno-fpu", "-mflat",
        "-mno-flat", "-mv8", "-mapp-regs", "-mno-app-regs", "-mfix-gr712rc",
        "-mfix-ut699", "-mfix-ut700", "-m32", "-m64", NULL
    };
    static const char *const prefix[] = {
        "-mcpu=", "-march=", "-mtune=", "-mcmodel=", NULL
    };
    if (!option_listed(arg, exact, prefix))
        return 0;
    /* The flags a LEON3 build passes (BCC's and clang's for
     * sparc bare metal). What EmbCC emits is ONE configuration --
     * SPARC V8 with LEON3's multiply and divide, register windows,
     * soft float, %g2-%g4 used as scratch -- so each flag either
     * says exactly that and is accepted, or asks for something else
     * and is refused by name: an object built for another would
     * link and then disagree with its callers about where a double
     * is or which registers survive a call. */
    const char *v = strchr(arg, '=');
    v = v ? v + 1 : "";
    if (strncmp(arg, "-mcpu=", 6) == 0 ||
        strncmp(arg, "-march=", 7) == 0 ||
        strncmp(arg, "-mtune=", 7) == 0) {
        static const char *const cores[] = {
            "leon3", "v8", "leon4", "gr712rc", "gr740", "ut699",
            "sparcleon3", "leon3v7"
        };
        int ok = 0;
        for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
            ok |= strcmp(v, cores[k]) == 0;
        if (!ok)
            diag_fatal(NULL, 0, "%s is not a SPARC V8 core with "
                       "hardware multiply and divide: EmbCC emits "
                       "LEON3 code (leon3, leon4, v8, gr712rc, "
                       "gr740, ut699)", arg);
    } else if (strcmp(arg, "-mhard-float") == 0 ||
               strcmp(arg, "-mfpu") == 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                   "soft-float SPARC code, which passes floating "
                   "point in the integer registers", arg);
    } else if (strcmp(arg, "-mflat") == 0) {
        diag_fatal(NULL, 0, "-mflat is not supported: EmbCC's "
                   "SPARC code uses register windows (save and "
                   "restore)");
    } else if (strcmp(arg, "-mno-app-regs") == 0) {
        diag_fatal(NULL, 0, "-mno-app-regs is not supported: "
                   "EmbCC's SPARC code uses %%g2-%%g4 as scratch "
                   "registers (-mapp-regs)");
    } else if (strcmp(arg, "-m64") == 0 ||
               (strncmp(arg, "-mcmodel=", 9) == 0 &&
                strcmp(v, "medlow") != 0)) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                   "32-bit SPARC V8 with absolute addresses "
                   "(sethi/or)", arg);
    } else if (strncmp(arg, "-mfix-", 6) == 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC applies "
                   "no LEON errata workarounds", arg);
    }
    return 1;
}
