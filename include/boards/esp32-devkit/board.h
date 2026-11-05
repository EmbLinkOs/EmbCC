/* <board.h> for an ESP32 development board (embcc --board=esp32-devkit):
 * the ESP32-WROOM-32 boards (DevKitC, DOIT DevKit V1 and their kin), the
 * ROM booting the program from flash with no second-stage bootloader.
 *
 * The console is UART0 at 115200, the board's USB serial port: stdout and
 * stderr (printf, puts, write) go there with "\n" sent as "\r\n", stdin
 * reads it. The clocks are the ROM's: the CPU and APB at the crystal's
 * 40 MHz. The pins are GPIO numbers; 34 to 39 are inputs only, 6 to 11
 * are the flash's. The same names are in every board's <board.h>. */
#ifndef EMBCC_BOARD_H
#define EMBCC_BOARD_H

#define BOARD_NAME "ESP32 DevKit"
#define BOARD_ESP32_DEVKIT 1
#define BOARD_LED 2                 /* the blue LED of the DevKit V1 boards */
#define BOARD_CONSOLE_BAUD 115200

enum { BOARD_INPUT, BOARD_OUTPUT, BOARD_INPUT_PULLUP };

#ifdef __cplusplus
extern "C" {
#endif
void board_led(int on);
void board_pin_mode(int pin, int mode);
void board_pin_write(int pin, int high);
int board_pin_read(int pin);
void board_delay_ms(unsigned long ms);
void board_delay_us(unsigned long us);
unsigned long board_millis(void);
unsigned long long board_micros(void);
void board_putc(char c);            /* the console, raw */
int board_getc(void);               /* a byte from the console, -1 when none waits */
#ifdef __cplusplus
}
#endif

/* code that must not wait for flash (an interrupt handler's): in IRAM */
#define IRAM_ATTR __attribute__((section(".iram1")))
#endif
