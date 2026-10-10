/* EmbCC's <util/delay.h>: busy-wait delays, avr-libc's interface.
 *
 *   #define F_CPU 16000000UL      before the #include, or -DF_CPU=...
 *   #include <util/delay.h>
 *   _delay_ms(250);
 *
 * Each waits AT LEAST the time asked at F_CPU, counted in loops of four
 * cycles (_delay_loop_2), and longer by the time interrupts take. With a
 * constant argument the count is worked out at compile time when the
 * optimizer runs (-O1 and up); at -O0 it is worked out at run time in
 * software floating point, which adds to the delay. avr-libc's version
 * needs __builtin_avr_delay_cycles, which EmbCC does not have; this is
 * the loop that builtin would emit, to within its four-cycle grain. */
#ifndef _UTIL_DELAY_H_
#define _UTIL_DELAY_H_ 1

#include <stdint.h>
#include <util/delay_basic.h>

#ifndef F_CPU
#warning "F_CPU is not defined for <util/delay.h>; 1 MHz is assumed"
#define F_CPU 1000000UL
#endif

/* Loops of 4 cycles for `__n` units of 1/__per seconds, rounded up. */
static __inline__ __attribute__((__always_inline__)) void
__emb_delay(double __n, double __per)
{
    uint32_t __loops =
        (uint32_t)(__n * ((double)(F_CPU) / (4.0 * __per)) + 0.999);
    while (__loops > 65535UL) {
        _delay_loop_2(0);              /* 65536 loops */
        __loops -= 65536UL;
    }
    if (__loops)
        _delay_loop_2((uint16_t)__loops);
}

static __inline__ __attribute__((__always_inline__)) void
_delay_us(double __us)
{
    __emb_delay(__us, 1e6);
}

static __inline__ __attribute__((__always_inline__)) void
_delay_ms(double __ms)
{
    __emb_delay(__ms, 1e3);
}

#endif /* _UTIL_DELAY_H_ */
