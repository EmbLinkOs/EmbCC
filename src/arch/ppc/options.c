/* PowerPC's command-line options: GCC's -m options for this target, each
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

int ppc_target_option(const char *arg)
{
    static const char *const exact[] = {
        "-msoft-float", "-mhard-float", "-mspe", "-mno-spe", "-mvle",
        "-mno-vle", "-meabi", "-mno-eabi", "-mlong-double-64",
        "-mlong-double-128", "-mno-isel", "-misel", "-msecure-plt",
        "-mno-sdata", NULL
    };
    static const char *const prefix[] = {
        "-mcpu=", "-msdata", "-mfloat-abi=", "-mabi=", "-G", NULL
    };
    if (!option_listed(arg, exact, prefix))
        return 0;
    /* The flags an e500/e200 build passes (gcc's powerpc-eabi and
     * clang's). What EmbCC emits is ONE configuration -- 32-bit
     * Book E PowerPC, the EABI, soft float, no SPE or VLE, a
     * 64-bit long double, no small data -- so each flag either
     * says that and is accepted, or asks for something else and
     * is refused by name: an object built for another would link
     * and then disagree with its callers about where a double is,
     * how wide a long double is, or what r2 and r13 hold. */
    const char *v = strchr(arg, '=');
    v = v ? v + 1 : "";
    if (strncmp(arg, "-mcpu=", 6) == 0) {
        static const char *const cores[] = {
            "e500", "8548", "e500v1", "e500v2", "8540", "e200",
            "e200z0", "e200z2", "e200z3", "e200z4", "e200z6",
            "e200z7", "ppc", "powerpc", "ppc32", "generic",
            "603", "603e", "e300c2", "e300c3", "750", "7400", "440"
        };
        int ok = 0;
        for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
            ok |= strcmp(v, cores[k]) == 0;
        if (!ok)
            diag_fatal(NULL, 0, "%s is not a 32-bit PowerPC core "
                       "EmbCC emits for: its code is 32-bit Book E "
                       "PowerPC without SPE (e500, 8548, e500v1, "
                       "e500v2, e200z0-z7, ppc, 603, 750, 440, ...)",
                       arg);
    } else if (strcmp(arg, "-mhard-float") == 0 ||
               (strncmp(arg, "-mfloat-abi=", 12) == 0 &&
                strcmp(v, "soft") != 0)) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                   "soft-float PowerPC code, which passes floating "
                   "point in the general registers", arg);
    } else if (strcmp(arg, "-mspe") == 0) {
        diag_fatal(NULL, 0, "-mspe is not supported: EmbCC emits no "
                   "SPE (signal processing engine) instructions; its "
                   "e500 code is soft float (-mno-spe)");
    } else if (strcmp(arg, "-mvle") == 0) {
        diag_fatal(NULL, 0, "-mvle is not supported: EmbCC emits "
                   "32-bit Book E instructions, not VLE");
    } else if (strcmp(arg, "-mlong-double-128") == 0) {
        diag_fatal(NULL, 0, "-mlong-double-128 is not supported: "
                   "EmbCC's PowerPC long double is the 8-byte double "
                   "(-mlong-double-64)");
    } else if (strcmp(arg, "-mno-eabi") == 0 ||
               strcmp(arg, "-msecure-plt") == 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC emits the "
                   "embedded ABI (-meabi) for bare metal", arg);
    } else if (strncmp(arg, "-mabi=", 6) == 0 &&
               strcmp(v, "ibmlongdouble") != 0 &&
               strcmp(v, "no-spe") != 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC emits the "
                   "PowerPC EABI with soft float", arg);
    } else if ((strncmp(arg, "-msdata", 7) == 0 &&
                strcmp(arg, "-msdata=none") != 0) ||
               (strncmp(arg, "-G", 2) == 0 &&
                strcmp(arg, "-G0") != 0)) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC puts no "
                   "data in small-data sections and addresses "
                   "nothing through r2 or r13 (-msdata=none, -G0)",
                   arg);
    }
    return 1;
}
