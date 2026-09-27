; The AVR harness's startup, for QEMU's arduino-uno (ATmega328P).
;
; Assembly, and not by preference: three of the five things a reset
; handler has to do on this machine have no C spelling at all.
;
;   The stack pointer starts at zero. AVR pushes by storing and then
;   decrementing, so the very first `push` of a C function compiled for
;   this part would write outside SRAM. SPL/SPH have to be set before any
;   call, which means before any C.
;
;   r1 must be zero. Every AVR object in the program -- EmbCC's and
;   avr-gcc's alike -- reads it as a source of zero bytes, and nothing
;   sets it but this.
;
;   .data and .rodata live in FLASH and have to be read with `lpm`, which
;   addresses program space. There is no C expression that emits it: a
;   pointer dereference compiles to `ld`, which reads the DATA space and
;   would return whatever SRAM happens to hold. That is the whole Harvard
;   problem, and copying the image across is how avr-gcc solves it too.
;
; The requirements document puts startup code in .S files and assembling
; them outside EmbCC's scope, so this is assembled by llvm-mc -- the same
; tool that refereed the instruction encoder. EmbCC's own assembler has no
; AVR support yet; when it does, this file is what it must accept.
;
; embld provides the brackets: __data_load is the flash address the
; writable image was stored at, __data_start/__data_end its RAM extent,
; and __bss_start/__bss_end the zeroed part.

	.text

; ---- the interrupt vector table -------------------------------------
;
; The ATmega328P's table is 26 entries at the bottom of flash, four bytes
; each -- a `jmp`, which is the only branch that reaches all of flash. Entry
; 0 is reset, so execution begins here whether the part was powered on or
; an interrupt fired.
;
; Every handler is declared WEAK. A program that uses three interrupts must
; still link, and an undefined weak reference resolves to zero -- which on
; this machine makes a spurious interrupt jump to the reset vector, exactly
; what avr-libc's __bad_interrupt does. A test defines only the vectors it
; needs; the names are avr-libc's, so an ISR() macro's handler lands in the
; right slot.
;
; The table must begin at address 0 and the ELF's ENTRY must be 0 as well:
; QEMU refuses an AVR image otherwise ("BIOS entry_point must be 0x0000"),
; and on real silicon the part fetches its first instruction from 0
; regardless. So the entry symbol is this table, not `reset`.
	.globl	__vectors
__vectors:
	.weak	__vector_1
	.weak	__vector_2
	.weak	__vector_3
	.weak	__vector_4
	.weak	__vector_5
	.weak	__vector_6
	.weak	__vector_7
	.weak	__vector_8
	.weak	__vector_9
	.weak	__vector_10
	.weak	__vector_11
	.weak	__vector_12
	.weak	__vector_13
	.weak	__vector_14
	.weak	__vector_15
	.weak	__vector_16
	.weak	__vector_17
	.weak	__vector_18
	.weak	__vector_19
	.weak	__vector_20
	.weak	__vector_21
	.weak	__vector_22
	.weak	__vector_23
	.weak	__vector_24
	.weak	__vector_25
	jmp	reset
	jmp	__vector_1
	jmp	__vector_2
	jmp	__vector_3
	jmp	__vector_4
	jmp	__vector_5
	jmp	__vector_6
	jmp	__vector_7
	jmp	__vector_8
	jmp	__vector_9
	jmp	__vector_10
	jmp	__vector_11
	jmp	__vector_12
	jmp	__vector_13
	jmp	__vector_14
	jmp	__vector_15
	jmp	__vector_16
	jmp	__vector_17
	jmp	__vector_18
	jmp	__vector_19
	jmp	__vector_20
	jmp	__vector_21
	jmp	__vector_22
	jmp	__vector_23
	jmp	__vector_24
	jmp	__vector_25

	.globl	reset
reset:
	; SP = RAMEND. The ATmega328P has 2 KB of SRAM ending at 0x08ff.
	ldi	r28, 0xff
	ldi	r29, 0x08
	out	0x3e, r29		; SPH
	out	0x3d, r28		; SPL
	; r1 = 0, the machine's zero register.
	eor	r1, r1
	; Clear SREG, so the first arithmetic does not inherit a carry.
	out	0x3f, r1

	; Copy the writable image (.data and, on this target, .rodata)
	; from flash to RAM. Z walks the flash, X the RAM.
	ldi	r30, lo8(__data_load)
	ldi	r31, hi8(__data_load)
	ldi	r26, lo8(__data_start)
	ldi	r27, hi8(__data_start)
	ldi	r18, lo8(__data_end)
	ldi	r19, hi8(__data_end)
1:
	cp	r26, r18
	cpc	r27, r19
	breq	2f
	lpm	r0, Z+
	st	X+, r0
	rjmp	1b
2:
	; Zero .bss.
	ldi	r26, lo8(__bss_start)
	ldi	r27, hi8(__bss_start)
	ldi	r18, lo8(__bss_end)
	ldi	r19, hi8(__bss_end)
3:
	cp	r26, r18
	cpc	r27, r19
	breq	4f
	st	X+, r1
	rjmp	3b
4:
	call	main
	; main returned. Nothing above this is the bottom of a call stack
	; on a bare part, so stop here rather than fall into whatever
	; follows in flash.
5:
	rjmp	5b
