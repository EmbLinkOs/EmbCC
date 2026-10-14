/* The data EEPROM functions of <avr/eeprom.h> (lib/avr), avr-libc's.
 * A read waits for a write in progress, loads EEAR and strobes EERE; a
 * write waits, loads EEAR and EEDR, and sets EEMPE then EEPE within four
 * cycles with interrupts off (one asm statement), as the datasheet's
 * "Atomic Byte Programming" requires. EEPM is left at its reset value,
 * erase and write in one operation. One object per EMB_AVR_ name. */
#include <avr/eeprom.h>
#include <avr/io.h>

uint8_t __emb_avr_ee_read(uint16_t a);
void __emb_avr_ee_write(uint16_t a, uint8_t v);

#ifdef EMB_AVR_EE_CORE
uint8_t __emb_avr_ee_read(uint16_t a)
{
    eeprom_busy_wait();
    EEAR = a;
    EECR |= (uint8_t)_BV(EERE);
    return EEDR;
}

void __emb_avr_ee_write(uint16_t a, uint8_t v)
{
    uint8_t sreg;
    eeprom_busy_wait();
    EEAR = a;
    EEDR = v;
    __asm__ __volatile__("in %0, __SREG__\n\t"
                         "cli\n\t"
                         "sbi %1, %2\n\t"
                         "sbi %1, %3\n\t"
                         "out __SREG__, %0"
                         : "=&r"(sreg)
                         : "I"(_SFR_IO_ADDR(EECR)), "I"(EEMPE), "I"(EEPE)
                         : "memory");
}
#endif

#define A(p) ((uint16_t)(p))

#ifdef EMB_AVR_EE_READ
uint8_t eeprom_read_byte(const uint8_t *p) { return __emb_avr_ee_read(A(p)); }

void eeprom_read_block(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    uint16_t a = A(src);
    while (n--)
        *d++ = __emb_avr_ee_read(a++);
}

uint16_t eeprom_read_word(const uint16_t *p)
{
    uint16_t v;
    eeprom_read_block(&v, p, sizeof v);
    return v;
}

uint32_t eeprom_read_dword(const uint32_t *p)
{
    uint32_t v;
    eeprom_read_block(&v, p, sizeof v);
    return v;
}

float eeprom_read_float(const float *p)
{
    float v;
    eeprom_read_block(&v, p, sizeof v);
    return v;
}
#endif

#ifdef EMB_AVR_EE_WRITE
void eeprom_write_byte(uint8_t *p, uint8_t value) { __emb_avr_ee_write(A(p), value); }

void eeprom_write_block(const void *src, void *dst, size_t n)
{
    const uint8_t *s = src;
    uint16_t a = A(dst);
    while (n--)
        __emb_avr_ee_write(a++, *s++);
}

void eeprom_write_word(uint16_t *p, uint16_t value) { eeprom_write_block(&value, p, sizeof value); }
void eeprom_write_dword(uint32_t *p, uint32_t value) { eeprom_write_block(&value, p, sizeof value); }
void eeprom_write_float(float *p, float value) { eeprom_write_block(&value, p, sizeof value); }
#endif

#ifdef EMB_AVR_EE_UPDATE
void eeprom_update_block(const void *src, void *dst, size_t n)
{
    const uint8_t *s = src;
    uint16_t a = A(dst);
    for (; n--; a++, s++)
        if (__emb_avr_ee_read(a) != *s)
            __emb_avr_ee_write(a, *s);
}

void eeprom_update_byte(uint8_t *p, uint8_t value) { eeprom_update_block(&value, p, 1); }
void eeprom_update_word(uint16_t *p, uint16_t value) { eeprom_update_block(&value, p, sizeof value); }
void eeprom_update_dword(uint32_t *p, uint32_t value) { eeprom_update_block(&value, p, sizeof value); }
void eeprom_update_float(float *p, float value) { eeprom_update_block(&value, p, sizeof value); }
#endif
