/* <board.h> for an Arduino Mega 2560 (embcc --board=mega2560):
 * an ATmega2560 at 16 MHz, programmed through its STK500v2 bootloader
 * (embflash --stk500v2).
 *
 * The console is the ATmega's UART at 115200, the board's USB serial port:
 * printf, puts and putchar send to it with "\n" as "\r\n", getchar reads
 * it. Timer1 is the board's clock (board_millis, an interrupt every
 * millisecond, so interrupts are on before main); Timer0 and Timer2 are
 * free; Timer3 to Timer5 too. The pins are the board's numbers: 0 to 53
 * the digital header, 54 to 69 A0 to A15. The same names are in every
 * board's <board.h>. */
#ifndef EMBCC_BOARD_H
#define EMBCC_BOARD_H

#define BOARD_NAME "Arduino Mega 2560"
#define BOARD_MEGA2560 1
#define BOARD_LED 13                /* the "L" LED, PB7 */
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
#endif
