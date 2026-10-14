/* EmbCC's <avr/pgmspace.h>: data in program memory, avr-libc's interface.
 *
 * PROGMEM puts a const static object in flash (.progmem.data), where it
 * takes no SRAM. Its type does not change, so the program reads it with
 * these macros, which use `lpm`; a plain `*p` would read SRAM at the same
 * number. EmbCC's own __flash qualifier does the same with the type
 * carrying the address space, and is what the macros read through.
 *
 *   const uint8_t table[] PROGMEM = { ... };
 *   uint8_t v = pgm_read_byte(&table[i]);
 *   uart_puts_P(PSTR("ready\n"));
 *
 * Every part EmbCC takes has at most 32 KB of flash, so every address is
 * near: the _near and plain forms are the same, and there are no _far ones
 * (those need RAMPZ and elpm). The string functions with a _P suffix take
 * one string in flash; they are in the AVR library (lib/avr/pgmspace.c). */
#ifndef __PGMSPACE_H_
#define __PGMSPACE_H_ 1

#include <stdint.h>
#include <stddef.h>
#include <avr/io.h>

#ifndef __ASSEMBLER__

#ifndef __ATTR_PROGMEM__
#define __ATTR_PROGMEM__ __attribute__((__progmem__))
#endif
#ifndef PROGMEM
#define PROGMEM __ATTR_PROGMEM__
#endif

/* A pointer to a string, or to anything, in flash. */
#define PGM_P      const char *
#define PGM_VOID_P const void *

/* A string literal kept in flash: its address, as a const char *. */
#define PSTR(s) (__extension__({                                            \
        static const char __pstr[] PROGMEM = (s);                           \
        &__pstr[0];                                                         \
    }))

/* Reads from flash at an address: a pointer or a 16-bit number. */
#define __EMB_PGM(type, addr) (*(const __flash type *)(uint16_t)(addr))

#define pgm_read_byte_near(addr)  __EMB_PGM(uint8_t, addr)
#define pgm_read_word_near(addr)  __EMB_PGM(uint16_t, addr)
#define pgm_read_dword_near(addr) __EMB_PGM(uint32_t, addr)
#define pgm_read_qword_near(addr) __EMB_PGM(uint64_t, addr)
#define pgm_read_float_near(addr) __EMB_PGM(float, addr)
#define pgm_read_ptr_near(addr)   ((void *)__EMB_PGM(uint16_t, addr))

#define pgm_read_byte(addr)  pgm_read_byte_near(addr)
#define pgm_read_word(addr)  pgm_read_word_near(addr)
#define pgm_read_dword(addr) pgm_read_dword_near(addr)
#define pgm_read_qword(addr) pgm_read_qword_near(addr)
#define pgm_read_float(addr) pgm_read_float_near(addr)
#define pgm_read_ptr(addr)   pgm_read_ptr_near(addr)

#define __LPM(addr)       pgm_read_byte(addr)
#define __LPM_word(addr)  pgm_read_word(addr)
#define __LPM_dword(addr) pgm_read_dword(addr)

/* Each takes its flash argument as a PGM_P / PGM_VOID_P; the other is in
 * SRAM. A result that points into a flash argument points into flash. */
extern void *memcpy_P(void *dst, PGM_VOID_P src, size_t n);
extern int memcmp_P(const void *s1, PGM_VOID_P s2, size_t n);
extern PGM_VOID_P memchr_P(PGM_VOID_P s, int c, size_t n);
extern size_t strlen_P(PGM_P s);
extern size_t strnlen_P(PGM_P s, size_t max);
extern int strcmp_P(const char *s1, PGM_P s2);
extern int strncmp_P(const char *s1, PGM_P s2, size_t n);
extern int strcasecmp_P(const char *s1, PGM_P s2);
extern int strncasecmp_P(const char *s1, PGM_P s2, size_t n);
extern char *strcpy_P(char *dst, PGM_P src);
extern char *strncpy_P(char *dst, PGM_P src, size_t n);
extern char *strcat_P(char *dst, PGM_P src);
extern char *strncat_P(char *dst, PGM_P src, size_t n);
extern PGM_P strchr_P(PGM_P s, int c);
extern PGM_P strrchr_P(PGM_P s, int c);
extern char *strstr_P(const char *s1, PGM_P s2);

#endif /* !__ASSEMBLER__ */

#endif /* __PGMSPACE_H_ */
