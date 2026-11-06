/* The ESP32's C startup (lib/esp32/crt0.S calls it on the program's own
 * stack): the watchdogs a flash boot leaves running off, .bss zeroed (the
 * ROM loaded .data itself, a segment of the image), the constructors --
 * a board's set-up among them, first (its object is linked first) --
 * then main, and exit with its value. */
#define REG(a) (*(volatile unsigned *)(a))
#define WDT_KEY 0x50D83AA1u

extern char __bss_start[], __bss_end[];
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
int main(void);
void exit(int status) __attribute__((noreturn));

void __embcc_esp32_start(void)
{
    /* the RTC watchdog and timer group 0's, each behind its write key */
    REG(0x3FF480A4u) = WDT_KEY; REG(0x3FF4808Cu) = 0; REG(0x3FF480A4u) = 0;
    REG(0x3FF5F064u) = WDT_KEY; REG(0x3FF5F048u) = 0; REG(0x3FF5F064u) = 0;
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
        (*f)();
    exit(main());
}
