; EmbCC's startup for the avr5 parts, crt<part>.o: what avr-libc's
; crt<part>.o and libgcc give an avr-gcc program, for EmbCC's.
;
; Assembled once per part by tools/build-rt.sh (embcc --target=avr
; -mmcu=PART -c crt.S); the driver links it first whenever -mmcu= names
; the part, as avr-gcc links avr-libc's, and not with -nostartfiles. The
; layout it expects is avr-ld's: lib/avr/avr5.ld, the script the driver
; uses when the link names none, or the program's own (avr-gcc's default
; avr5 script and the ones derived from it).
;
;   .vectors   __vectors, the part's table at address 0: a jmp to __init,
;              then one jmp per vector to __vector_N. Each __vector_N is a
;              WEAK symbol defined here at __bad_interrupt, so a program's
;              own __vector_N (ISR() names it) takes its place, and any
;              other jumps to __bad_interrupt, which jumps to
;              __vector_default -- a program's ISR(BADISR_vect), or else
;              address 0, a restart.
;   .init0     __init, where reset lands (weak: a program may replace the
;              whole sequence).
;   .init2     r1 = 0, SREG = 0, SP = __stack (weak: RAMEND unless the
;              link defines it).
;   .init4     __do_copy_data: .data (and .rodata, which EmbCC reads from
;              RAM) from its stored copy in flash, with lpm; then
;              __do_clear_bss: .bss to zero. libgcc's names, so objects
;              from avr-gcc, which reference them, link too.
;   .init6     __do_global_ctors: each function in .init_array, in order
;              (EmbCC's __attribute__((constructor))), when the script
;              brackets it with __init_array_start/__init_array_end.
;   .init9     call main, then exit.
;   .fini9     _exit, and exit as a weak name for it: avr-libc's exit is
;              the same. .fini8 to .fini1 run here; .fini0 disables
;              interrupts and stops in a loop.
;
; A program adds to the sequence by putting naked code in .init1, .init3,
; .init5, .init7, .init8 (avr-libc's way of clearing MCUSR and the watchdog
; before .data is copied, for one), or .fini8 to .fini1: the script lays
; .init0 to .init9 out in order with nothing between, so each falls
; through to the next. None of it runs with a stack frame or with r1
; known to be zero before .init2.

#include <avr/io.h>

	.section .vectors,"ax",@progbits
	.globl	__vectors
__vectors:
	jmp	__init
	.weak	__vector_1
	.set	__vector_1, __bad_interrupt
	jmp	__vector_1
	.weak	__vector_2
	.set	__vector_2, __bad_interrupt
	jmp	__vector_2
	.weak	__vector_3
	.set	__vector_3, __bad_interrupt
	jmp	__vector_3
	.weak	__vector_4
	.set	__vector_4, __bad_interrupt
	jmp	__vector_4
	.weak	__vector_5
	.set	__vector_5, __bad_interrupt
	jmp	__vector_5
	.weak	__vector_6
	.set	__vector_6, __bad_interrupt
	jmp	__vector_6
	.weak	__vector_7
	.set	__vector_7, __bad_interrupt
	jmp	__vector_7
	.weak	__vector_8
	.set	__vector_8, __bad_interrupt
	jmp	__vector_8
	.weak	__vector_9
	.set	__vector_9, __bad_interrupt
	jmp	__vector_9
	.weak	__vector_10
	.set	__vector_10, __bad_interrupt
	jmp	__vector_10
	.weak	__vector_11
	.set	__vector_11, __bad_interrupt
	jmp	__vector_11
	.weak	__vector_12
	.set	__vector_12, __bad_interrupt
	jmp	__vector_12
	.weak	__vector_13
	.set	__vector_13, __bad_interrupt
	jmp	__vector_13
	.weak	__vector_14
	.set	__vector_14, __bad_interrupt
	jmp	__vector_14
	.weak	__vector_15
	.set	__vector_15, __bad_interrupt
	jmp	__vector_15
	.weak	__vector_16
	.set	__vector_16, __bad_interrupt
	jmp	__vector_16
	.weak	__vector_17
	.set	__vector_17, __bad_interrupt
	jmp	__vector_17
	.weak	__vector_18
	.set	__vector_18, __bad_interrupt
	jmp	__vector_18
	.weak	__vector_19
	.set	__vector_19, __bad_interrupt
	jmp	__vector_19
	.weak	__vector_20
	.set	__vector_20, __bad_interrupt
	jmp	__vector_20
	.weak	__vector_21
	.set	__vector_21, __bad_interrupt
	jmp	__vector_21
	.weak	__vector_22
	.set	__vector_22, __bad_interrupt
	jmp	__vector_22
	.weak	__vector_23
	.set	__vector_23, __bad_interrupt
	jmp	__vector_23
	.weak	__vector_24
	.set	__vector_24, __bad_interrupt
	jmp	__vector_24
	.weak	__vector_25
	.set	__vector_25, __bad_interrupt
	jmp	__vector_25
#if _VECTORS_SIZE > 104
	; the ATmega1280's and 2560's: 57 vectors (their datasheet's
	; "Interrupts", <avr/iomxx0_1.h>)
	.weak	__vector_26
	.set	__vector_26, __bad_interrupt
	jmp	__vector_26
	.weak	__vector_27
	.set	__vector_27, __bad_interrupt
	jmp	__vector_27
	.weak	__vector_28
	.set	__vector_28, __bad_interrupt
	jmp	__vector_28
	.weak	__vector_29
	.set	__vector_29, __bad_interrupt
	jmp	__vector_29
	.weak	__vector_30
	.set	__vector_30, __bad_interrupt
	jmp	__vector_30
	.weak	__vector_31
	.set	__vector_31, __bad_interrupt
	jmp	__vector_31
	.weak	__vector_32
	.set	__vector_32, __bad_interrupt
	jmp	__vector_32
	.weak	__vector_33
	.set	__vector_33, __bad_interrupt
	jmp	__vector_33
	.weak	__vector_34
	.set	__vector_34, __bad_interrupt
	jmp	__vector_34
	.weak	__vector_35
	.set	__vector_35, __bad_interrupt
	jmp	__vector_35
	.weak	__vector_36
	.set	__vector_36, __bad_interrupt
	jmp	__vector_36
	.weak	__vector_37
	.set	__vector_37, __bad_interrupt
	jmp	__vector_37
	.weak	__vector_38
	.set	__vector_38, __bad_interrupt
	jmp	__vector_38
	.weak	__vector_39
	.set	__vector_39, __bad_interrupt
	jmp	__vector_39
	.weak	__vector_40
	.set	__vector_40, __bad_interrupt
	jmp	__vector_40
	.weak	__vector_41
	.set	__vector_41, __bad_interrupt
	jmp	__vector_41
	.weak	__vector_42
	.set	__vector_42, __bad_interrupt
	jmp	__vector_42
	.weak	__vector_43
	.set	__vector_43, __bad_interrupt
	jmp	__vector_43
	.weak	__vector_44
	.set	__vector_44, __bad_interrupt
	jmp	__vector_44
	.weak	__vector_45
	.set	__vector_45, __bad_interrupt
	jmp	__vector_45
	.weak	__vector_46
	.set	__vector_46, __bad_interrupt
	jmp	__vector_46
	.weak	__vector_47
	.set	__vector_47, __bad_interrupt
	jmp	__vector_47
	.weak	__vector_48
	.set	__vector_48, __bad_interrupt
	jmp	__vector_48
	.weak	__vector_49
	.set	__vector_49, __bad_interrupt
	jmp	__vector_49
	.weak	__vector_50
	.set	__vector_50, __bad_interrupt
	jmp	__vector_50
	.weak	__vector_51
	.set	__vector_51, __bad_interrupt
	jmp	__vector_51
	.weak	__vector_52
	.set	__vector_52, __bad_interrupt
	jmp	__vector_52
	.weak	__vector_53
	.set	__vector_53, __bad_interrupt
	jmp	__vector_53
	.weak	__vector_54
	.set	__vector_54, __bad_interrupt
	jmp	__vector_54
	.weak	__vector_55
	.set	__vector_55, __bad_interrupt
	jmp	__vector_55
	.weak	__vector_56
	.set	__vector_56, __bad_interrupt
	jmp	__vector_56
#endif

	.text
	.globl	__bad_interrupt
__bad_interrupt:
	.weak	__vector_default
	.set	__vector_default, __vectors
	jmp	__vector_default

	.section .init0,"ax",@progbits
	.weak	__init
__init:
	.weak	__stack
	.set	__stack, RAMEND

	.section .init2,"ax",@progbits
	clr	r1
	out	_SFR_IO_ADDR(SREG), r1
	ldi	r28, lo8(__stack)
	ldi	r29, hi8(__stack)
	out	_SFR_IO_ADDR(SPH), r29
	out	_SFR_IO_ADDR(SPL), r28

	.section .init4,"ax",@progbits
	.globl	__do_copy_data
__do_copy_data:
	ldi	r30, lo8(__data_load_start)
	ldi	r31, hi8(__data_load_start)
	ldi	r26, lo8(__data_start)
	ldi	r27, hi8(__data_start)
	ldi	r18, lo8(__data_end)
	ldi	r19, hi8(__data_end)
	rjmp	2f
1:
	lpm	r0, Z+
	st	X+, r0
2:
	cp	r26, r18
	cpc	r27, r19
	brne	1b
	.globl	__do_clear_bss
__do_clear_bss:
	ldi	r26, lo8(__bss_start)
	ldi	r27, hi8(__bss_start)
	ldi	r18, lo8(__bss_end)
	ldi	r19, hi8(__bss_end)
	rjmp	4f
3:
	st	X+, r1
4:
	cp	r26, r18
	cpc	r27, r19
	brne	3b

	.section .init6,"ax",@progbits
	.globl	__do_global_ctors
__do_global_ctors:
	; The table is in RAM by now (the script puts .init_array in .data),
	; one word address per function. r16:r17 and r28:r29 walk it: the
	; callees preserve them, and nothing before main needs them.
	.weak	__init_array_start
	.weak	__init_array_end
	ldi	r16, lo8(__init_array_start)
	ldi	r17, hi8(__init_array_start)
	ldi	r28, lo8(__init_array_end)
	ldi	r29, hi8(__init_array_end)
	rjmp	6f
5:
	movw	r26, r16
	ld	r30, X+
	ld	r31, X+
	movw	r16, r26
	icall
6:
	cp	r16, r28
	cpc	r17, r29
	brne	5b

	.section .init9,"ax",@progbits
	call	main
	jmp	exit

	.section .fini9,"ax",@progbits
	.globl	_exit
	.weak	exit
	.set	exit, _exit
_exit:

	.section .fini0,"ax",@progbits
	cli
__stop_program:
	rjmp	__stop_program
