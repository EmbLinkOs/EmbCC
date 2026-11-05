/* <board.h> for an Arduino Uno or Nano (embcc --board=uno, --board=nano):
 * an ATmega328P at 16 MHz, programmed through Optiboot (embflash
 * --stk500).
 *
 * The console is the ATmega's UART at 115200, the board's USB serial port:
 * printf, puts and putchar send to it with "\n" as "\r\n", getchar reads
 * it. Timer1 is the board's clock (board_millis, an interrupt every
 * millisecond, so interrupts are on before main); Timer0 and Timer2 are
 * free. The pins are the board's numbers: 0 to 13 the digital header, 14
 * to 19 A0 to A5. The same names are in every board's <board.h>. */
#ifndef EMBCC_BOARD_H
#define EMBCC_BOARD_H

#ifdef BOARD_NANO                   /* --board=nano: the same part and pins */
#define BOARD_NAME "Arduino Nano"
#else
#define BOARD_NAME "Arduino Uno"
#define BOARD_UNO 1
#endif
#define BOARD_LED 13                /* the "L" LED, PB5 */
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
