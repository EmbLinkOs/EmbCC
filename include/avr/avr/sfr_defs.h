/* EmbCC's <avr/sfr_defs.h>: how a special function register is named.
 *
 * The interface is avr-libc's, so that code written against <avr/io.h>
 * compiles unchanged; the text is EmbCC's own. A register is a volatile
 * lvalue at its DATA-space address: `PORTB |= _BV(PB5);` is an ordinary
 * read-modify-write, which EmbCC turns into sbi/cbi where the address is
 * in the low I/O range and into lds/sts elsewhere.
 *
 * The register file has two addresses for the first 64 I/O registers: the
 * I/O address that `in`, `out`, `sbi` and `cbi` take (0x00-0x3f), and the
 * data-space address every other instruction uses, 0x20 higher. The part
 * headers name each register by the address the datasheet gives:
 * _SFR_IO8(io) for the first kind, _SFR_MEM8(data) for the extended I/O
 * range above 0x5f, which only the data space reaches.
 *
 * In an assembly file (__ASSEMBLER__) a register is its number instead:
 * the data-space address, as avr-libc has it with its default
 * __SFR_OFFSET, so `out _SFR_IO_ADDR(PORTB), r24` and `sts UDR0, r24`.
 *
 * _SFR_MEM_ADDR() and _SFR_IO_ADDR() take a register and give its
 * address. EmbCC folds them where a constant is needed by the code -- an
 * asm "I" or "n" operand, a static initializer -- but, unlike GCC, not
 * in an integer constant expression (a case label, _Static_assert). */
#ifndef _AVR_SFR_DEFS_H_
#define _AVR_SFR_DEFS_H_

#ifndef __SFR_OFFSET
#define __SFR_OFFSET 0x20
#endif

#ifdef __ASSEMBLER__

#define _SFR_MEM8(mem_addr)  (mem_addr)
#define _SFR_MEM16(mem_addr) (mem_addr)
#define _SFR_MEM32(mem_addr) (mem_addr)
#define _SFR_IO8(io_addr)    ((io_addr) + __SFR_OFFSET)
#define _SFR_IO16(io_addr)   ((io_addr) + __SFR_OFFSET)
#define _SFR_MEM_ADDR(sfr)   (sfr)
#define _SFR_IO_ADDR(sfr)    ((sfr) - __SFR_OFFSET)
#define _SFR_IO_REG_P(sfr)   ((sfr) < 0x40 + __SFR_OFFSET)

#else /* C */

#include <stdint.h>

#define _MMIO_BYTE(mem_addr)  (*(volatile uint8_t *)(mem_addr))
#define _MMIO_WORD(mem_addr)  (*(volatile uint16_t *)(mem_addr))
#define _MMIO_DWORD(mem_addr) (*(volatile uint32_t *)(mem_addr))

#define _SFR_MEM8(mem_addr)  _MMIO_BYTE(mem_addr)
#define _SFR_MEM16(mem_addr) _MMIO_WORD(mem_addr)
#define _SFR_MEM32(mem_addr) _MMIO_DWORD(mem_addr)
#define _SFR_IO8(io_addr)    _MMIO_BYTE((io_addr) + __SFR_OFFSET)
#define _SFR_IO16(io_addr)   _MMIO_WORD((io_addr) + __SFR_OFFSET)

#define _SFR_MEM_ADDR(sfr)   ((uint16_t) &(sfr))
#define _SFR_IO_ADDR(sfr)    (_SFR_MEM_ADDR(sfr) - __SFR_OFFSET)
#define _SFR_IO_REG_P(sfr)   (_SFR_MEM_ADDR(sfr) < 0x40 + __SFR_OFFSET)

#define _SFR_ADDR(sfr) _SFR_MEM_ADDR(sfr)
#define _SFR_BYTE(sfr) _MMIO_BYTE(_SFR_ADDR(sfr))
#define _SFR_WORD(sfr) _MMIO_WORD(_SFR_ADDR(sfr))

#endif /* __ASSEMBLER__ */

/* A bit's mask, and the tests and waits written with it. */
#define _BV(bit) (1 << (bit))

#ifndef __ASSEMBLER__
#define bit_is_set(sfr, bit)   (_SFR_BYTE(sfr) & _BV(bit))
#define bit_is_clear(sfr, bit) (!(_SFR_BYTE(sfr) & _BV(bit)))
#define loop_until_bit_is_set(sfr, bit) \
    do { } while (bit_is_clear(sfr, bit))
#define loop_until_bit_is_clear(sfr, bit) \
    do { } while (bit_is_set(sfr, bit))
#endif

/* The name of interrupt vector N's handler, which the startup's table
 * jumps to (lib/avr/crt.S). */
#define _VECTOR(N) __vector_ ## N

#endif /* _AVR_SFR_DEFS_H_ */
