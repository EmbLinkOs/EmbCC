/* EmbCC's <util/delay_basic.h>: counted busy loops, avr-libc's names.
 *
 * _delay_loop_1(n) takes 3 cycles per count, _delay_loop_2(n) 4; a count
 * of 0 is 256 and 65536 counts.
 *
 * The branch back is `brne .-4`: `.±N` counts from the end of the branch,
 * as GNU as and llvm-mc read it, so this is the instruction before it. */
#ifndef _UTIL_DELAY_BASIC_H_
#define _UTIL_DELAY_BASIC_H_ 1

#include <stdint.h>

static __inline__ __attribute__((__always_inline__)) void
_delay_loop_1(uint8_t __count)
{
    __asm__ __volatile__("dec %0\n\t"
                         "brne .-4"
                         : "+r"(__count));
}

static __inline__ __attribute__((__always_inline__)) void
_delay_loop_2(uint16_t __count)
{
    __asm__ __volatile__("sbiw %0, 1\n\t"
                         "brne .-4"
                         : "+w"(__count));
}

#endif /* _UTIL_DELAY_BASIC_H_ */
