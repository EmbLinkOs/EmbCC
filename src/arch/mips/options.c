/* MIPS32 and MIPS64's command-line options: GCC's -m options for this target, each
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

int mips64_target_option(const char *arg)
{
    static const char *const exact[] = {
        "-msoft-float", "-mhard-float", "-mno-abicalls", "-mabicalls", "-EL",
        "-EB", NULL
    };
    static const char *const prefix[] = {
        "-mcpu=", "-march=", "-mabi=", "-G", NULL
    };
    if (!option_listed(arg, exact, prefix))
        return 0;
    /* MIPS64's one configuration, as MIPS32's below: MIPS64
     * Release 2, n64, soft float, no abicalls, no small data, in
     * the triple's byte order. */
    const char *v = strchr(arg, '=');
    v = v ? v + 1 : "";
    if (strncmp(arg, "-mcpu=", 6) == 0 ||
        strncmp(arg, "-march=", 7) == 0) {
        static const char *const cores[] = {
            "mips64r2", "5kc", "5kf", "5kec", "5kef", "octeon"
        };
        int ok = 0;
        for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
            ok |= strcmp(v, cores[k]) == 0;
        if (!ok)
            diag_fatal(NULL, 0, "%s is not a MIPS64 Release 2 core: "
                       "EmbCC emits MIPS64r2 (mips64r2, 5kc, 5kf, "
                       "5kec, 5kef, octeon)", arg);
    } else if (strncmp(arg, "-mabi=", 6) == 0) {
        if (strcmp(v, "64") != 0)
            diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                       "the n64 ABI (-mabi=64) only", arg);
    } else if (strcmp(arg, "-mhard-float") == 0) {
        diag_fatal(NULL, 0, "-mhard-float is not supported: EmbCC "
                   "emits soft-float n64, which passes floating "
                   "point in the integer registers");
    } else if (strcmp(arg, "-mabicalls") == 0) {
        diag_fatal(NULL, 0, "-mabicalls is not supported: EmbCC's "
                   "MIPS64 code takes addresses absolutely "
                   "(%%highest..%%lo) and keeps no $gp; it is "
                   "-mno-abicalls code");
    } else if (strcmp(arg, "-EB") == 0 && !target_big_endian()) {
        diag_fatal(NULL, 0, "-EB contradicts --target=%s, which is "
                   "little-endian: big-endian MIPS64 is "
                   "--target=mips64-none-elf", target_triple_now());
    } else if (strcmp(arg, "-EL") == 0 && target_big_endian()) {
        diag_fatal(NULL, 0, "-EL contradicts --target=%s, which is "
                   "big-endian: little-endian MIPS64 is "
                   "--target=mips64el-none-elf", target_triple_now());
    } else if (strncmp(arg, "-G", 2) == 0 &&
               strcmp(arg, "-G0") != 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC puts no "
                   "data in .sdata and addresses nothing through "
                   "$gp (-G0)", arg);
    }
    return 1;
}

int mips32_target_option(const char *arg)
{
    static const char *const exact[] = {
        "-msoft-float", "-mhard-float", "-mno-abicalls", "-mabicalls", "-EL",
        "-EB", NULL
    };
    static const char *const prefix[] = {
        "-mcpu=", "-march=", "-mabi=", "-G", NULL
    };
    if (!option_listed(arg, exact, prefix))
        return 0;
    /* The flags a MIPS build passes (a PIC32 project's, clang's
     * and gcc's for mipsel bare metal). What EmbCC emits is ONE
     * configuration -- MIPS32 Release 2, o32, soft float, no
     * abicalls, no small data, in the triple's byte order (-EL
     * mipsel, -EB mips) -- so each flag either
     * says exactly that and is accepted, or asks for something
     * else and is refused by name: an object built for another
     * of these would link and then disagree with its callers
     * about registers, byte order or the global pointer. */
    const char *v = strchr(arg, '=');
    v = v ? v + 1 : "";
    if (strncmp(arg, "-mcpu=", 6) == 0 ||
        strncmp(arg, "-march=", 7) == 0) {
        static const char *const cores[] = {
            "mips32r2", "m4k", "m14k", "m14kc", "24kc", "24kf",
            "24kec", "24kef", "34kc", "74kc"
        };
        int ok = 0;
        for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
            ok |= strcmp(v, cores[k]) == 0;
        if (!ok)
            diag_fatal(NULL, 0, "%s is not a MIPS32 Release 2 core: "
                       "EmbCC emits MIPS32r2 (mips32r2, m4k, m14k, "
                       "m14kc, 24kc, 24kf, 24kec, 24kef, 34kc, 74kc)",
                       arg);
    } else if (strncmp(arg, "-mabi=", 6) == 0) {
        if (strcmp(v, "32") != 0)
            diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                       "the o32 ABI (-mabi=32) only", arg);
    } else if (strcmp(arg, "-mhard-float") == 0) {
        diag_fatal(NULL, 0, "-mhard-float is not supported: EmbCC "
                   "emits soft-float o32, which passes floating "
                   "point in the integer registers");
    } else if (strcmp(arg, "-mabicalls") == 0) {
        diag_fatal(NULL, 0, "-mabicalls is not supported: EmbCC's "
                   "MIPS code takes addresses absolutely (lui/addiu) "
                   "and keeps no $gp; it is -mno-abicalls code");
    } else if (strcmp(arg, "-EB") == 0 && !target_big_endian()) {
        diag_fatal(NULL, 0, "-EB contradicts --target=%s, which is "
                   "little-endian: big-endian MIPS is "
                   "--target=mips-none-elf", target_triple_now());
    } else if (strcmp(arg, "-EL") == 0 && target_big_endian()) {
        diag_fatal(NULL, 0, "-EL contradicts --target=%s, which is "
                   "big-endian: little-endian MIPS is "
                   "--target=mipsel-none-elf", target_triple_now());
    } else if (strncmp(arg, "-G", 2) == 0 &&
               strcmp(arg, "-G0") != 0) {
        diag_fatal(NULL, 0, "%s is not supported: EmbCC puts no "
                   "data in .sdata and addresses nothing through "
                   "$gp (-G0)", arg);
    }
    return 1;
}
