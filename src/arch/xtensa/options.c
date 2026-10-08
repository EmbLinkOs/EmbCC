/* Xtensa's command-line options: GCC's -m options for this target, each
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

/* GCC's Xtensa options (gcc/config/xtensa/xtensa.opt and elf.opt, and
 * Espressif's), which the Xtensa target answers itself. The -m spellings
 * every target accepts are not among them. */
static int xtensa_flag(const char *a)
{
    static const char *const exact[] = {
        "-mlongcalls", "-mno-longcalls", "-mtext-section-literals",
        "-mno-text-section-literals", "-mauto-litpools", "-mno-auto-litpools",
        "-mserialize-volatile", "-mno-serialize-volatile", "-mtarget-align",
        "-mno-target-align", "-mforce-no-pic", "-mlittle-endian",
        "-mbig-endian", "-mstrict-align", "-mno-strict-align", "-mlra",
        "-mno-lra", "-mconst16", "-mno-const16", "-mforce-l32",
        "-mno-fix-esp32-psram-cache-issue", NULL
    };
    static const char *const prefix[] = {
        "-mabi=", "-mdynconfig=", "-mextra-l32r-costs=",
        "-mfix-esp32-psram-cache-issue", NULL
    };
    return option_listed(a, exact, prefix);
}

int xtensa_target_option(const char *arg)
{
    if (!(xtensa_flag(arg)))
        return 0;
    /* The flags an ESP-IDF build passes, and the rest of GCC's
     * xtensa.opt. What EmbCC emits is ONE configuration -- the
     * windowed ABI, little-endian, literals in .text before each
     * function, direct call8s, memw before every volatile access
     * -- so a flag that says that (or only changes how GCC would
     * have placed or costed the same code) is accepted, and one
     * that asks for anything else is refused by name. */
    static const char *const ok[] = {
        "-mlongcalls", "-mno-longcalls", "-mtext-section-literals",
        "-mno-text-section-literals", "-mauto-litpools",
        "-mno-auto-litpools", "-mserialize-volatile",
        "-mno-serialize-volatile", "-mtarget-align",
        "-mno-target-align", "-mforce-no-pic", "-mabi=windowed",
        "-mlittle-endian", "-mstrict-align", "-mno-strict-align",
        "-mlra", "-mno-lra", "-mno-fix-esp32-psram-cache-issue",
        "-mno-const16"
    };
    int good = 0;
    for (unsigned k = 0; k < sizeof ok / sizeof ok[0]; k++)
        good |= strcmp(arg, ok[k]) == 0;
    if (!strncmp(arg, "-mextra-l32r-costs=", 19))
        good = 1;           /* GCC's cost model alone */
    if (!strncmp(arg, "-mdynconfig=", 12) &&
        (strstr(arg, "esp32.so") || strstr(arg, "esp32s3.so")))
        good = 1;
    if (!good) {
        if (!strncmp(arg, "-mabi=", 6) &&
            strcmp(arg, "-mabi=call0"))
            diag_fatal(NULL, 0, "%s is not an Xtensa ABI: EmbCC "
                       "emits the windowed ABI (-mabi=windowed)",
                       arg);
        if (!strcmp(arg, "-mabi=call0"))
            diag_fatal(NULL, 0, "-mabi=call0 is not supported: EmbCC "
                       "emits the windowed ABI (call8/entry/retw), "
                       "which ESP-IDF uses");
        if (!strcmp(arg, "-mbig-endian"))
            diag_fatal(NULL, 0, "-mbig-endian is not supported: the "
                       "Xtensa target is little-endian only");
        if (!strncmp(arg, "-mfix-esp32-psram-cache-issue", 29))
            diag_fatal(NULL, 0, "%s is not supported: EmbCC does not "
                       "insert the ESP32 rev. 1 PSRAM workaround",
                       arg);
        diag_fatal(NULL, 0, "%s is not supported for xtensa-none-elf: "
                   "EmbCC emits the windowed ABI for the ESP32 "
                   "(LX6) and ESP32-S3 (LX7), little-endian, with "
                   "literals before each function", arg);
    }
    return 1;
}
