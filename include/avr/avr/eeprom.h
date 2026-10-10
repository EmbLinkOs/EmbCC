/* EmbCC's <avr/eeprom.h>: the data EEPROM, avr-libc's interface.
 *
 *   uint8_t boots EEMEM;                      in .eeprom, not in SRAM
 *   uint8_t n = eeprom_read_byte(&boots);
 *   eeprom_update_byte(&boots, n + 1);        writes only if it changed
 *
 * An EEMEM object's address is its EEPROM address; reading it with `*`
 * reads SRAM. Each function waits for a write still in progress first, and
 * a write returns once the part has started it (3.3 ms, the datasheet's
 * programming time), so the next access waits for it. Interrupts are
 * held off for the four cycles of the EEMPE/EEPE sequence alone. The
 * functions are in the AVR library (lib/avr/eeprom.c). */
#ifndef _AVR_EEPROM_H_
#define _AVR_EEPROM_H_ 1

#include <avr/io.h>

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

#ifndef EEMEM
#define EEMEM __attribute__((__section__(".eeprom")))
#endif

#define eeprom_is_ready() bit_is_clear(EECR, EEPE)
#define eeprom_busy_wait() do { } while (!eeprom_is_ready())

uint8_t eeprom_read_byte(const uint8_t *p);
uint16_t eeprom_read_word(const uint16_t *p);
uint32_t eeprom_read_dword(const uint32_t *p);
float eeprom_read_float(const float *p);
void eeprom_read_block(void *dst, const void *src, size_t n);

void eeprom_write_byte(uint8_t *p, uint8_t value);
void eeprom_write_word(uint16_t *p, uint16_t value);
void eeprom_write_dword(uint32_t *p, uint32_t value);
void eeprom_write_float(float *p, float value);
void eeprom_write_block(const void *src, void *dst, size_t n);

void eeprom_update_byte(uint8_t *p, uint8_t value);
void eeprom_update_word(uint16_t *p, uint16_t value);
void eeprom_update_dword(uint32_t *p, uint32_t value);
void eeprom_update_float(float *p, float value);
void eeprom_update_block(const void *src, void *dst, size_t n);

#endif /* !__ASSEMBLER__ */

#endif /* _AVR_EEPROM_H_ */
