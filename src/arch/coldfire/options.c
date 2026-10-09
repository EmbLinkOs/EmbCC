/* ColdFire's command-line options: GCC's -m options for this target, each
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

int coldfire_target_option(const char *arg)
{
    static const char *const exact[] = {
        "-msoft-float", "-mhard-float", "-mdiv", "-mno-div", "-malign-int",
        "-mno-align-int", "-mshort", "-mno-short", "-mstrict-align",
        "-mno-strict-align", "-mpcrel", "-mid-shared-library", "-msep-data",
        "-mxgot", "-mrtd", "-mno-rtd", NULL
    };
    static const char *const prefix[] = {
        "-mcpu=", "-march=", "-mtune=", "-m5", "-m68", "-mc68", "-mcpu32",
        "-mcfv", NULL
    };
    if (!option_listed(arg, exact, prefix))
        return 0;
    /* The flags a ColdFire build passes (GCC's m68k-elf, which
     * selects ColdFire with -mcpu=). What EmbCC emits is ONE
     * configuration -- ColdFire ISA_A with the hardware divide,
     * soft float, int 32 bits and 2-aligned, the caller popping
     * its arguments, absolute addresses -- so each flag either says
     * that and is accepted, or asks for something else and is
     * refused by name: an object built otherwise would link and
     * then disagree with its callers about where an argument is,
     * how wide an int is or what an instruction means. */
    const char *v = strchr(arg, '=');
    v = v ? v + 1 : "";
    if (strncmp(arg, "-mcpu=", 6) == 0 ||
        strncmp(arg, "-mtune=", 7) == 0 ||
        strncmp(arg, "-m5", 3) == 0) {
        /* the ISA_A-or-later cores with a divider and no FPU */
        static const char *const cores[] = {
            "5208", "5207", "5206e", "5210a", "5211a", "5211",
            "5212", "5213", "5214", "5216", "5221x", "52221",
            "52223", "52230", "52231", "52232", "52233", "52234",
            "52235", "5224", "5225", "52252", "52254", "52255",
            "52256", "52258", "52259", "52274", "52277", "5232",
            "5233", "5234", "5235", "523x", "5249", "5250", "5253",
            "5270", "5271", "5272", "5274", "5275", "5280", "5281",
            "5282", "528x", "5307", "5327", "5328", "5329", "532x",
            "5372", "5373", "537x", "5407", "54410", "54415",
            "54416", "54417", "54418", "54450", "54451", "54452",
            "54453", "54454", "54455"
        };
        const char *c = strncmp(arg, "-m5", 3) == 0
                      ? arg + 2 : v;
        int ok = 0;
        for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
            ok |= strcmp(c, cores[k]) == 0;
        if (!ok && (strcmp(c, "5206") == 0 || strcmp(c, "5202") == 0 ||
                    strcmp(c, "5204") == 0))
            diag_fatal(NULL, 0, "%s is not supported: that core has "
                       "no hardware divide, and EmbCC's ColdFire "
                       "code divides with divs.l/divu.l", arg);
        if (!ok && strncmp(c, "547", 3) != 0 &&
            strncmp(c, "548", 3) != 0)
            diag_fatal(NULL, 0, "%s is not a ColdFire core EmbCC "
                       "emits for: its code is ColdFire ISA_A with the "
                       "hardware divide (5208, 5213, 5235, 5282, 5329, "
                       "5407, 54455, ...); the 68000 family proper is "
                       "not a target", arg);
        if (!ok)
            diag_fatal(NULL, 0, "%s is not supported: that core's "
                       "FPU makes GCC pass and return floating point "
                       "in its registers, and EmbCC's ColdFire code "
                       "is soft float", arg);
    } else if (strncmp(arg, "-march=", 7) == 0) {
        if (strcmp(v, "isaa") != 0 && strcmp(v, "isaaplus") != 0 &&
            strcmp(v, "isab") != 0 && strcmp(v, "isac") != 0)
            diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                       "ColdFire ISA_A (-march=isaa), which isaaplus, "
                       "isab and isac cores also run", arg);
    } else if (strncmp(arg, "-m68", 4) == 0 ||
               strncmp(arg, "-mc68", 5) == 0 ||
               strncmp(arg, "-mcpu32", 7) == 0 ||
               strncmp(arg, "-mcfv", 5) == 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC's m68k "
                   "target is ColdFire ISA_A (-mcpu=5208 and its "
                   "kin); the 68000 family proper is not a target",
                   arg);
    } else if (strcmp(arg, "-mhard-float") == 0) {
        diag_fatal(NULL, 0, "-mhard-float is not supported: EmbCC "
                   "emits soft-float ColdFire code, which passes "
                   "floating point in the data registers");
    } else if (strcmp(arg, "-mno-div") == 0) {
        diag_fatal(NULL, 0, "-mno-div is not supported: EmbCC's "
                   "ColdFire code divides with divs.l/divu.l");
    } else if (strcmp(arg, "-malign-int") == 0) {
        diag_fatal(NULL, 0, "-malign-int is not supported: EmbCC "
                   "lays out the m68k's 2-byte alignment, which "
                   "GCC's ColdFire code has without it");
    } else if (strcmp(arg, "-mshort") == 0) {
        diag_fatal(NULL, 0, "-mshort is not supported: EmbCC's int "
                   "is 32 bits on the m68k");
    } else if (strcmp(arg, "-mrtd") == 0) {
        diag_fatal(NULL, 0, "-mrtd is not supported: EmbCC's caller "
                   "pops its arguments (the SVR4 m68k convention)");
    } else if (strcmp(arg, "-mpcrel") == 0 ||
               strcmp(arg, "-mid-shared-library") == 0 ||
               strcmp(arg, "-msep-data") == 0 ||
               strcmp(arg, "-mxgot") == 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC's ColdFire "
                   "code takes every address absolutely", arg);
    }
    return 1;
}
