/* RX's command-line options: GCC's -m options for this target, each
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

int rx_target_option(const char *arg)
{
    static const char *const exact[] = {
        "-m64bit-doubles", "-nofpu", "-mnofpu", "-fpu",
        "-mlittle-endian-data", "-mbig-endian-data", "-mrx-abi", "-mgcc-abi",
        "-mpid", "-mno-pid", "-mallow-string-insns",
        "-mno-allow-string-insns", "-mas100-syntax", "-mrelax", NULL
    };
    static const char *const prefix[] = {
        "-mcpu=", "-m32bit-doubles", "-msmall-data-limit=",
        "-mint-register=", "-mmax-constant-size=", NULL
    };
    if (!option_listed(arg, exact, prefix))
        return 0;
    /* The flags an RX build passes (GCC's rx-elf ones). EmbCC
     * emits ONE configuration -- RXv1, little-endian data, GCC's
     * RX ABI with 32-bit doubles, no FPU instructions -- so each
     * flag either says that and is accepted, or asks for another
     * and is refused by name: an object built otherwise would
     * link and then disagree with its callers about doubles, byte
     * order or a reserved register. */
    const char *a = arg;
    if (strncmp(a, "-mcpu=", 6) == 0) {
        const char *v = a + 6;
        if (strcmp(v, "rx600") && strcmp(v, "rx610") &&
            strcmp(v, "rx200") && strcmp(v, "rx100") &&
            strcmp(v, "RX600") && strcmp(v, "RX610") &&
            strcmp(v, "RX200") && strcmp(v, "RX100"))
            diag_fatal(NULL, 0, "%s is not an RXv1 core: EmbCC "
                       "emits the RXv1 instruction set (rx600, "
                       "rx610, rx200, rx100)", a);
    } else if (!strcmp(a, "-m64bit-doubles")) {
        diag_fatal(NULL, 0, "-m64bit-doubles is not supported: EmbCC "
                   "emits GCC's rx-elf default, -m32bit-doubles "
                   "(double is binary32), and a 64-bit double "
                   "changes the ABI of every double");
    } else if (!strcmp(a, "-fpu")) {
        diag_fatal(NULL, 0, "-fpu is not supported: EmbCC's RX code "
                   "is soft float (-nofpu); the RX600 FPU "
                   "instructions are not emitted yet");
    } else if (!strcmp(a, "-mbig-endian-data")) {
        diag_fatal(NULL, 0, "-mbig-endian-data is not supported: the "
                   "RX target is little-endian only");
    } else if (!strcmp(a, "-mgcc-abi")) {
        diag_fatal(NULL, 0, "-mgcc-abi is not supported: EmbCC "
                   "passes stacked arguments naturally aligned, "
                   "GCC's default -mrx-abi");
    } else if (strncmp(a, "-msmall-data-limit=", 19) == 0 &&
               strcmp(a + 19, "0") != 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC puts no data "
                   "in a small-data area addressed from a base "
                   "register", a);
    } else if (!strcmp(a, "-mpid")) {
        diag_fatal(NULL, 0, "-mpid is not supported: EmbCC's RX code "
                   "addresses data absolutely, not position-"
                   "independently");
    } else if (strncmp(a, "-mint-register=", 15) == 0 &&
               strcmp(a + 15, "0") != 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC reserves no "
                   "registers for interrupt handlers", a);
    } else if (!strcmp(a, "-mno-allow-string-insns")) {
        diag_fatal(NULL, 0, "-mno-allow-string-insns is not "
                   "supported: EmbCC copies large blocks with smovf "
                   "and sstr");
    } else if (!strcmp(a, "-mas100-syntax")) {
        diag_fatal(NULL, 0, "-mas100-syntax is not supported: EmbCC "
                   "writes objects, not Renesas AS100 assembly");
    }
    return 1;
}
