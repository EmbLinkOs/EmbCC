/* EmbCC's <avr/common.h>: the registers every AVR has in the same place,
 * the stack pointer and the status register, with avr-libc's names.
 * <avr/io.h> includes it. */
#ifndef _AVR_COMMON_H_
#define _AVR_COMMON_H_

#include <avr/sfr_defs.h>

/* The stack pointer, as a pair and as its bytes (I/O 0x3d and 0x3e). */
#ifndef __ASSEMBLER__
#define SP      _SFR_IO16(0x3D)
#endif
#define SPL     _SFR_IO8(0x3D)
#define SPH     _SFR_IO8(0x3E)
#define SP0     0
#define SP1     1
#define SP2     2
#define SP3     3
#define SP4     4
#define SP5     5
#define SP6     6
#define SP7     7
#define SP8     0
#define SP9     1
#define SP10    2

/* The status register (I/O 0x3f), and its flags by bit. */
#define SREG    _SFR_IO8(0x3F)
#define SREG_C  0
#define SREG_Z  1
#define SREG_N  2
#define SREG_V  3
#define SREG_S  4
#define SREG_H  5
#define SREG_T  6
#define SREG_I  7

/* The same, as numbers: the I/O and data addresses, and the names the
 * register file gives the two registers the ABI reserves. */
#define AVR_STATUS_REG          SREG
#define AVR_STATUS_ADDR         _SFR_IO_ADDR(SREG)
#define AVR_STACK_POINTER_REG   SP
#define AVR_STACK_POINTER_LO_REG SPL
#define AVR_STACK_POINTER_HI_REG SPH
#define AVR_STACK_POINTER_LO_ADDR _SFR_IO_ADDR(SPL)
#define AVR_STACK_POINTER_HI_ADDR _SFR_IO_ADDR(SPH)

#endif /* _AVR_COMMON_H_ */
