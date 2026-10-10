/* AVR's command-line options: avr-gcc's -m options, each accepted when it
 * says what EmbCC emits and refused by name when it asks for something
 * else. The driver asks through the backend registry (src/arch/backends.c,
 * `option`).
 *
 * -mmcu= names the part. The backend's code is the ATmega328P's avr5 core
 * (a 2-byte program counter, `jmp` and `call`, `mul`, `movw`, `lpm Rd, Z+`),
 * so -mmcu= takes the parts that ARE that core and differ from the
 * ATmega328P only in their memories: the rest of its datasheet's family
 * with jmp/call. Each part's numbers are the datasheet's (the ATmega48A/PA/
 * 88A/PA/168A/PA/328/P datasheet, "Memories" and "Interrupts"), and the
 * part's own predefined macro is clang's for the same -mmcu=. The ATmega48
 * and ATmega88 are in that datasheet too, and are refused: they are avr4,
 * one-word vectors and no jmp or call, which every EmbCC call is.
 *
 * Without -mmcu= nothing changes: the target is still the ATmega328P, as it
 * was before the option existed.
 *
 * Kept free of the registry's helpers (option_listed): embls links
 * src/arch/predef.c, which asks target_avr_mcu, and not backends.c. */
#include "../backend.h"
#include "../target.h"
#include "../../driver/util.h"

#include <string.h>

static const struct avr_mcu avr_mcus[] = {
    /* name          macro                  flash   RAMEND  EEPROM vectors */
    { "atmega328p", "__AVR_ATmega328P__", 32768, 0x08ff, 1024, 26 },
    { "atmega328",  "__AVR_ATmega328__",  32768, 0x08ff, 1024, 26 },
    { "atmega168p", "__AVR_ATmega168P__", 16384, 0x04ff,  512, 26 },
    { "atmega168",  "__AVR_ATmega168__",  16384, 0x04ff,  512, 26 },
};

static const struct avr_mcu *g_mcu;

const struct avr_mcu *target_avr_mcu(void)
{
    return target_get() == TARGET_AVR ? g_mcu : NULL;
}

/* The same datasheet's avr4 parts: the same peripherals, but rjmp/rcall
 * only and a one-word vector table. */
static int avr4_sibling(const char *v)
{
    static const char *const avr4[] = {
        "atmega48", "atmega48a", "atmega48p", "atmega48pa",
        "atmega88", "atmega88a", "atmega88p", "atmega88pa", NULL
    };
    for (int i = 0; avr4[i]; i++)
        if (strcmp(v, avr4[i]) == 0)
            return 1;
    return 0;
}

static void set_mcu(const char *arg, const char *v)
{
    for (unsigned i = 0; i < sizeof avr_mcus / sizeof avr_mcus[0]; i++)
        if (strcmp(v, avr_mcus[i].name) == 0) {
            g_mcu = &avr_mcus[i];
            return;
        }
    if (avr4_sibling(v))
        diag_fatal(NULL, 0, "%s is not supported: the %s is an avr4 part, "
                   "with no jmp or call instruction, and EmbCC's AVR code "
                   "calls with `call` (it generates for the avr5 core: "
                   "-mmcu=atmega328p, atmega328, atmega168p or atmega168)",
                   arg, v);
    diag_fatal(NULL, 0, "%s is not supported: EmbCC's AVR backend generates "
               "code for the ATmega328P's avr5 core, and knows the memories "
               "and vectors of atmega328p, atmega328, atmega168p and "
               "atmega168 alone", arg);
}

int avr_target_option(const char *arg)
{
    if (strncmp(arg, "-mmcu=", 6) == 0) {
        set_mcu(arg, arg + 6);
        return 1;
    }
    /* Hints a compiler may decline, as GCC's documentation describes
     * them: both make an image SMALLER and neither changes what it does
     * or how it calls. -mrelax lets the linker shorten a call or jmp to
     * rcall or rjmp where the target is near; EmbLD does not, so every
     * call stays a 4-byte `call`. -mcall-prologues saves and restores
     * registers through shared library routines (__prologue_saves__);
     * EmbCC writes each prologue in the function, which avr-gcc's
     * objects call and are called by all the same. */
    if (!strcmp(arg, "-mrelax") || !strcmp(arg, "-mno-relax") ||
        !strcmp(arg, "-mcall-prologues") || !strcmp(arg, "-mno-call-prologues"))
        return 1;
    /* The size of double and long double: 32 bits is what EmbCC has (and
     * avr-gcc's default); 64 would change every double in the ABI. */
    if (!strcmp(arg, "-mdouble=32") || !strcmp(arg, "-mlong-double=32"))
        return 1;
    if (!strcmp(arg, "-mdouble=64") || !strcmp(arg, "-mlong-double=64"))
        diag_fatal(NULL, 0, "%s is not supported: double and long double "
                   "are binary32 on this target, the same type as float, "
                   "and a 64-bit double changes the ABI of every one", arg);
    if (!strcmp(arg, "-mint8"))
        diag_fatal(NULL, 0, "-mint8 is not supported: int is 16 bits on "
                   "this target, and an 8-bit int changes the type of every "
                   "integer expression and the ABI of every call");
    return 0;
}
