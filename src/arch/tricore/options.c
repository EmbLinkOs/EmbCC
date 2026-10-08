/* TriCore's command-line options: GCC's -m options for this target, each
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

int tricore_target_option(const char *arg)
{
    static const char *const exact[] = {
        "-msoft-float", "-mhard-float", "-mlittle-endian", NULL
    };
    static const char *const prefix[] = {
        "-mcpu=", "-march=", "-mtc", NULL
    };
    if (!option_listed(arg, exact, prefix))
        return 0;
    /* The flags a TriCore build passes (HighTec's GCC spells the
     * core -mcpu=tc27xx or -mtc161). What EmbCC emits is ONE
     * configuration -- TriCore 1.6.1 instructions, which every
     * TC2xx and TC3xx core executes, and soft float -- so each
     * flag either names a core that runs it and is accepted, or
     * asks for something else and is refused by name. */
    const char *v = strchr(arg, '=');
    v = v ? v + 1 : arg + 2;          /* -mtc161: "tc161" */
    if (strcmp(arg, "-mhard-float") == 0)
        diag_fatal(NULL, 0, "-mhard-float is not supported: EmbCC "
                   "emits soft float for TriCore (the TC3xx FPU is "
                   "not used yet)");
    if (strcmp(arg, "-msoft-float") && strcmp(arg,
                                                 "-mlittle-endian")) {
        static const char *const cores[] = {
            "tc16", "tc161", "tc162", "tc1.6", "tc1.6.1", "tc1.6.2",
            "tc16x", "tc2xx", "tc22xx", "tc23xx", "tc26xx", "tc27xx",
            "tc29xx", "tc3xx", "tc33xx", "tc36xx", "tc37xx",
            "tc38xx", "tc39xx"
        };
        int ok = 0;
        for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
            ok |= strcmp(v, cores[k]) == 0;
        if (!ok)
            diag_fatal(NULL, 0, "%s is not a TriCore 1.6 core: EmbCC "
                       "emits TriCore 1.6.1 code, for the AURIX "
                       "TC2xx and TC3xx (tc16, tc161, tc162, tc27xx, "
                       "tc37xx, ...)", arg);
    }
    return 1;
}
