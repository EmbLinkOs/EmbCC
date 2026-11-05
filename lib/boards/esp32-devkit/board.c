/* The ESP32 DevKit's runtime (embcc --board=esp32-devkit links it after the
 * startup): the console on UART0, the pins through the GPIO matrix and
 * IO_MUX, and the time from timer group 0's 64-bit counter at 1 MHz -- no
 * interrupt needed. Registers are the ESP32 Technical Reference Manual's. */
#include <board.h>

#define REG(a) (*(volatile unsigned *)(a))
#define UART0_FIFO      REG(0x3FF40000u)
#define UART0_CLKDIV    REG(0x3FF40014u)
#define UART0_STATUS    REG(0x3FF4001Cu)
#define GPIO_OUT_W1TS   REG(0x3FF44008u)
#define GPIO_OUT_W1TC   REG(0x3FF4400Cu)
#define GPIO_OUT1_W1TS  REG(0x3FF44014u)
#define GPIO_OUT1_W1TC  REG(0x3FF44018u)
#define GPIO_ENABLE_W1TS  REG(0x3FF44024u)
#define GPIO_ENABLE_W1TC  REG(0x3FF44028u)
#define GPIO_ENABLE1_W1TS REG(0x3FF44030u)
#define GPIO_ENABLE1_W1TC REG(0x3FF44034u)
#define GPIO_IN         REG(0x3FF4403Cu)
#define GPIO_IN1        REG(0x3FF44040u)
#define GPIO_FUNC_OUT_SEL(n) REG(0x3FF44530u + 4u * (unsigned)(n))
#define TIMG0_T0CONFIG  REG(0x3FF5F000u)
#define TIMG0_T0LO      REG(0x3FF5F004u)
#define TIMG0_T0HI      REG(0x3FF5F008u)
#define TIMG0_T0UPDATE  REG(0x3FF5F00Cu)

/* each pin's IO_MUX register, as an offset from 0x3FF49000 (0: none) */
static const unsigned char iomux[40] = {
    0x44, 0x88, 0x40, 0x84, 0x48, 0x6c, 0x60, 0x64, 0x68, 0x54,
    0x58, 0x5c, 0x34, 0x38, 0x30, 0x3c, 0x4c, 0x50, 0x70, 0x74,
    0x78, 0x7c, 0x80, 0x8c, 0x90, 0x24, 0x28, 0x2c, 0, 0,
    0, 0, 0x1c, 0x20, 0x14, 0x18, 0x04, 0x08, 0x0c, 0x10,
};
#define MCU_SEL_GPIO (2u << 12)
#define FUN_IE (1u << 9)
#define FUN_WPU (1u << 8)
#define FUN_WPD (1u << 7)

void board_putc(char c)
{
    while (((UART0_STATUS >> 16) & 0xff) >= 126)
        ;
    UART0_FIFO = (unsigned char)c;
}

int board_getc(void)
{
    if ((UART0_STATUS & 0xff) == 0)
        return -1;
    return (int)(UART0_FIFO & 0xff);
}

/* stdout and stderr: the console, "\n" as "\r\n" (lib/libc's bare-metal
 * backend has these weak: this object's are the program's) */
long write(int fd, const void *buf, unsigned long n)
{
    const char *p = buf;
    if (fd != 1 && fd != 2)
        return -1;
    for (unsigned long i = 0; i < n; i++) {
        if (p[i] == '\n')
            board_putc('\r');
        board_putc(p[i]);
    }
    return (long)n;
}

/* stdin: at least one byte, waited for, then what else has come */
long read(int fd, void *buf, unsigned long n)
{
    unsigned char *p = buf;
    unsigned long k = 0;
    int c;
    if (fd != 0)
        return -1;
    if (n == 0)
        return 0;
    while ((c = board_getc()) < 0)
        ;
    p[k++] = (unsigned char)c;
    while (k < n && (c = board_getc()) >= 0)
        p[k++] = (unsigned char)c;
    return (long)k;
}

void board_pin_mode(int pin, int mode)
{
    if (pin < 0 || pin > 39 || !iomux[pin])
        return;
    volatile unsigned *mux = (volatile unsigned *)(0x3FF49000u + iomux[pin]);
    unsigned v = (*mux & ~((7u << 12) | FUN_WPU | FUN_WPD)) | MCU_SEL_GPIO | FUN_IE;
    if (mode == BOARD_INPUT_PULLUP)
        v |= FUN_WPU;
    *mux = v;
    if (mode == BOARD_OUTPUT && pin < 34) {
        GPIO_FUNC_OUT_SEL(pin) = 0x100;     /* the pin's own output register */
        if (pin < 32) GPIO_ENABLE_W1TS = 1u << pin; else GPIO_ENABLE1_W1TS = 1u << (pin - 32);
    } else if (pin < 32) {
        GPIO_ENABLE_W1TC = 1u << pin;
    } else {
        GPIO_ENABLE1_W1TC = 1u << (pin - 32);
    }
}

void board_pin_write(int pin, int high)
{
    if (pin < 0 || pin > 33)
        return;
    if (pin < 32) {
        if (high) GPIO_OUT_W1TS = 1u << pin; else GPIO_OUT_W1TC = 1u << pin;
    } else {
        if (high) GPIO_OUT1_W1TS = 1u << (pin - 32); else GPIO_OUT1_W1TC = 1u << (pin - 32);
    }
}

int board_pin_read(int pin)
{
    if (pin < 0 || pin > 39)
        return 0;
    return pin < 32 ? (int)((GPIO_IN >> pin) & 1) : (int)((GPIO_IN1 >> (pin - 32)) & 1);
}

void board_led(int on)
{
    board_pin_write(BOARD_LED, on);
}

unsigned long long board_micros(void)
{
    TIMG0_T0UPDATE = 1;                 /* latch the counter */
    unsigned lo = TIMG0_T0LO, hi = TIMG0_T0HI;
    return (unsigned long long)hi << 32 | lo;
}

unsigned long board_millis(void)
{
    return (unsigned long)(board_micros() / 1000u);
}

void board_delay_us(unsigned long us)
{
    unsigned long long until = board_micros() + us;
    while (board_micros() < until)
        ;
}

void board_delay_ms(unsigned long ms)
{
    board_delay_us(ms * 1000ul);
}

/* Before main (the startup runs this object's constructor first): the
 * counter at 1 MHz -- APB / APB-in-MHz, APB read off UART0's divider, which
 * the ROM set for 115200 -- and the LED's pin an output. */
__attribute__((constructor)) static void board_init(void)
{
    unsigned d = UART0_CLKDIV;
    unsigned long long apb = ((unsigned long long)(d & 0xFFFFFu) * 16u + ((d >> 20) & 15u)) *
                             115200u / 16u;
    unsigned mhz = (unsigned)((apb + 500000u) / 1000000u);
    if (mhz < 2)
        mhz = 40;
    TIMG0_T0CONFIG = 0;
    TIMG0_T0CONFIG = (1u << 31) | (1u << 30) | ((mhz & 0xFFFFu) << 13);   /* on, up, APB / mhz */
    board_pin_mode(BOARD_LED, BOARD_OUTPUT);
}
