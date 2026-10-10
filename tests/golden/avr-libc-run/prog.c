/* tests/golden/avr-libc-run.sh: data in flash read back with pgm_read_*
 * and the _P functions, and the printf family, on QEMU's ATmega328P. */
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdio.h>
#include <string.h>

const uint8_t bytes[] PROGMEM = { 0x11, 0x22, 0x33, 0xfe };
const uint16_t words[] PROGMEM = { 0x1234, 0xbeef };
const uint32_t dwords[] PROGMEM = { 0x89abcdefUL };
const uint64_t qwords[] PROGMEM = { 0x0102030405060708ULL };
const float floats[] PROGMEM = { 2.5f };
const char s1[] PROGMEM = "Hello, flash";
const char s2[] PROGMEM = "world";
const char *const names[] PROGMEM = { s1, s2 };

static void putch(char c)
{
    loop_until_bit_is_set(UCSR0A, UDRE0);
    UDR0 = (uint8_t)c;
}

static void puts_ram(const char *s) { while (*s) putch(*s++); }

static char line[120];

#define SHOW(...) do { snprintf(line, sizeof line, __VA_ARGS__); puts_ram(line); putch('\n'); } while (0)

int main(void)
{
    char buf[24];
    UBRR0 = 8;
    UCSR0B = _BV(TXEN0);
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);

    /* pgm_read_* */
    SHOW("byte %x %x word %x %x", pgm_read_byte(&bytes[1]), pgm_read_byte(&bytes[3]),
         pgm_read_word(&words[0]), pgm_read_word(&words[1]));
    SHOW("dword %lx qword %llx float %d", (unsigned long)pgm_read_dword(&dwords[0]),
         (unsigned long long)pgm_read_qword(&qwords[0]), (int)(pgm_read_float(&floats[0]) * 2));
    SHOW("ptr %s", (const char *)strcpy_P(buf, (PGM_P)pgm_read_ptr(&names[1])));
    SHOW("near %x %x", pgm_read_byte_near(bytes), (unsigned)__LPM(&bytes[2]));

    /* the _P functions */
    memcpy_P(buf, s1, 5);
    buf[5] = 0;
    SHOW("memcpy_P %s strlen_P %u strnlen_P %u", buf, (unsigned)strlen_P(s1), (unsigned)strnlen_P(s1, 4));
    SHOW("strcmp_P %d %d %d", strcmp_P("world", s2) == 0, strcmp_P("worlc", s2) < 0, strcmp_P("worle", s2) > 0);
    SHOW("strncmp_P %d strcasecmp_P %d strncasecmp_P %d", strncmp_P("worxx", s2, 3) == 0,
         strcasecmp_P("WORLD", s2) == 0, strncasecmp_P("HELLO!", s1, 5) == 0);
    SHOW("memcmp_P %d %d", memcmp_P("Hello", s1, 5) == 0, memcmp_P("Hellp", s1, 5) > 0);
    strcpy_P(buf, s2);
    strcat_P(buf, s2);
    SHOW("strcpy_P+strcat_P %s", buf);
    strncpy_P(buf, s1, 3);
    buf[3] = 0;
    strncat_P(buf, s2, 2);
    SHOW("strncpy_P+strncat_P %s", buf);
    SHOW("strchr_P %d strrchr_P %d memchr_P %d", (int)(strchr_P(s1, 'l') - s1),
         (int)(strrchr_P(s1, 'l') - s1), (int)((const char *)memchr_P(s1, ',', 12) - s1));
    SHOW("strstr_P %s", strstr_P("say world!", s2));

    /* the printf family, against the host's C library */
#define F(...) SHOW(__VA_ARGS__);
#include "fmt.h"
#undef F
    /* avr-libc's own: formats in flash, %S, floating point as "?" */
    snprintf_P(line, sizeof line, PSTR("P:%d %S %s|%5.1f|"), 12, s2, "ram", 1.5);
    puts_ram(line);
    putch('\n');
    sprintf_P(line, PSTR("sprintf_P %u"), 99u);
    puts_ram(line);
    putch('\n');
    {
        int n = snprintf(buf, 6, "%s", "truncated");
        SHOW("snprintf %d %s", n, buf);
    }
    puts_ram("==END==\n");
    for (;;) {
    }
}
