/* EmbCC's <avr/io.h>: the selected part's registers, bits, vectors and
 * memories, by the names of its datasheet and of avr-libc.
 *
 * The part is the one -mmcu= names (src/arch/avr/options.c), told to this
 * header by its predefined macro; without -mmcu= it is the ATmega328P.
 * These headers are on the include path for the AVR target alone. */
#ifndef _AVR_IO_H_
#define _AVR_IO_H_

#include <avr/sfr_defs.h>

/* The memories, from the datasheet: the last flash byte, the first and
 * last SRAM bytes in the data space, the last EEPROM byte, its page, and
 * the self-programming page in bytes. There is no external RAM. */
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega328__)
#define FLASHEND     0x7FFF
#define RAMSTART     0x0100
#define RAMEND       0x08FF
#define E2END        0x03FF
#define E2PAGESIZE   4
#define SPM_PAGESIZE 128
#elif defined(__AVR_ATmega168P__) || defined(__AVR_ATmega168__)
#define FLASHEND     0x3FFF
#define RAMSTART     0x0100
#define RAMEND       0x04FF
#define E2END        0x01FF
#define E2PAGESIZE   4
#define SPM_PAGESIZE 128
#elif defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)
#if defined(__AVR_ATmega2560__)
#define FLASHEND     0x3FFFF
#else
#define FLASHEND     0x1FFFF
#endif
#define RAMSTART     0x0200
#define RAMEND       0x21FF
#define E2END        0x0FFF
#define E2PAGESIZE   8
#define SPM_PAGESIZE 256
#else
#error "<avr/io.h>: EmbCC has the registers of atmega328p, atmega328, atmega168p, atmega168, atmega1280 and atmega2560 (-mmcu=)"
#endif
#define XRAMSIZE 0
#define XRAMEND  RAMEND

/* The signature bytes the part answers a programmer with. */
#define SIGNATURE_0 0x1E
#if defined(__AVR_ATmega328P__)
#define SIGNATURE_1 0x95
#define SIGNATURE_2 0x0F
#elif defined(__AVR_ATmega328__)
#define SIGNATURE_1 0x95
#define SIGNATURE_2 0x14
#elif defined(__AVR_ATmega168P__)
#define SIGNATURE_1 0x94
#define SIGNATURE_2 0x0B
#elif defined(__AVR_ATmega2560__)
#define SIGNATURE_1 0x98
#define SIGNATURE_2 0x01
#elif defined(__AVR_ATmega1280__)
#define SIGNATURE_1 0x97
#define SIGNATURE_2 0x03
#else
#define SIGNATURE_1 0x94
#define SIGNATURE_2 0x06
#endif

#if defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)
#include <avr/iomxx0_1.h>
#else
#include <avr/iomx8.h>
#endif
#include <avr/common.h>

#endif /* _AVR_IO_H_ */
