/* EmbCC's <avr/cpufunc.h>: single instructions, avr-libc's names. */
#ifndef _AVR_CPUFUNC_H_
#define _AVR_CPUFUNC_H_

#ifndef __ASSEMBLER__
/* One cycle doing nothing, which the compiler keeps. */
#define _NOP() __asm__ __volatile__("nop")
/* No memory access is moved across it; no instruction is emitted. */
#define _MemoryBarrier() __asm__ __volatile__("" ::: "memory")
#endif

#endif /* _AVR_CPUFUNC_H_ */
