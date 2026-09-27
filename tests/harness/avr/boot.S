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
