; avr-isa.S -- the instruction kernels avr-isa.c sweeps: each runs one
; instruction (or a few) with SREG as the caller gives it, and returns
; the result and SREG after. avr-gcc's convention: the arguments in r24,
; r22 and r20; an 8-bit result in r24, 16 bits in r25:r24, 32 in
; r25:r22; r1 is zero again on return, and SREG is put back to 0 so the
; caller's code does not inherit a flag.
;
; Assembled by EmbCC (embcc --target=avr -c), whose encoder llvm-mc
; referees in avr-encoding.sh.

	.text

; ---- two registers: r24 OP r22, SREG r20 -> r24 result, r25 SREG ------------
	.macro OP2 name, insn
	.globl \name
\name:
	out 0x3f, r20
	\insn r24, r22
	in r25, 0x3f
	out 0x3f, r1
	ret
	.endm
	OP2 k_add, add
	OP2 k_adc, adc
	OP2 k_sub, sub
	OP2 k_sbc, sbc
	OP2 k_and, and
	OP2 k_or, or
	OP2 k_eor, eor
	OP2 k_cp, cp
	OP2 k_cpc, cpc
	OP2 k_mov, mov

; ---- an immediate: r24 OP K, SREG r20 ---------------------------------------
	.macro OPI name, insn, k
	.globl \name
\name:
	out 0x3f, r20
	\insn r24, \k
	in r25, 0x3f
	out 0x3f, r1
	ret
	.endm
	OPI k_subi_00, subi, 0x00
	OPI k_subi_01, subi, 0x01
	OPI k_subi_80, subi, 0x80
	OPI k_subi_ff, subi, 0xff
	OPI k_sbci_00, sbci, 0x00
	OPI k_sbci_01, sbci, 0x01
	OPI k_sbci_80, sbci, 0x80
	OPI k_sbci_ff, sbci, 0xff
	OPI k_cpi_00, cpi, 0x00
	OPI k_cpi_7f, cpi, 0x7f
	OPI k_cpi_80, cpi, 0x80
	OPI k_cpi_ff, cpi, 0xff
	OPI k_andi_0f, andi, 0x0f
	OPI k_andi_80, andi, 0x80
	OPI k_ori_01, ori, 0x01
	OPI k_ori_80, ori, 0x80
	OPI k_ldi_a5, ldi, 0xa5

; ---- one register: OP r24, SREG r20 -----------------------------------------
	.macro OP1 name, insn
	.globl \name
\name:
	out 0x3f, r20
	\insn r24
	in r25, 0x3f
	out 0x3f, r1
	ret
	.endm
	OP1 k_com, com
	OP1 k_neg, neg
	OP1 k_swap, swap
	OP1 k_inc, inc
	OP1 k_dec, dec
	OP1 k_asr, asr
	OP1 k_lsr, lsr
	OP1 k_ror, ror
	OP1 k_lsl, lsl
	OP1 k_rol, rol
	OP1 k_tst, tst
	OP1 k_clr, clr
	OP1 k_ser, ser

; ---- the multiplies: r22 = R0, r23 = R1, r24 = SREG (a macro is not named
; after an instruction: the names are not case-sensitive) -------------------
	.macro KMUL name, insn
	.globl \name
\name:
	mov r18, r24
	mov r19, r22
	out 0x3f, r20
	\insn r18, r19
	in r24, 0x3f
	movw r22, r0
	clr r1
	ldi r25, 0
	out 0x3f, r1
	ret
	.endm
	KMUL k_mul, mul
	KMUL k_muls, muls
	KMUL k_mulsu, mulsu
	KMUL k_fmul, fmul
	KMUL k_fmuls, fmuls
	KMUL k_fmulsu, fmulsu

; ---- 16 bits: r25:r24 OP K, SREG r22 -> r23:r22 result, r24 SREG -------------
	.macro W16 name, insn, k
	.globl \name
\name:
	out 0x3f, r22
	\insn r24, \k
	in r20, 0x3f
	movw r22, r24
	mov r24, r20
	ldi r25, 0
	out 0x3f, r1
	ret
	.endm
	W16 k_adiw_0, adiw, 0
	W16 k_adiw_1, adiw, 1
	W16 k_adiw_63, adiw, 63
	W16 k_sbiw_0, sbiw, 0
	W16 k_sbiw_1, sbiw, 1
	W16 k_sbiw_63, sbiw, 63

; ---- skips: a count of what was not skipped -----------------------------------
inc18:
	inc r18
	ret

; CPSE over one word, a two-word JMP and a two-word CALL: r24, r22
	.globl k_cpse
k_cpse:
	clr r18
	cpse r24, r22
	inc r18
	cpse r24, r22
	jmp k_cpse_1
	subi r18, 0xf0
k_cpse_1:
	cpse r24, r22
	call inc18
	mov r24, r18
	ret

; SBRC and SBRS on each bit of r24, over one- and two-word instructions
	.globl k_sbrx
k_sbrx:
	clr r18
	sbrc r24, 0
	inc r18
	sbrs r24, 1
	subi r18, 0xfe
	sbrc r24, 2
	call inc18
	sbrs r24, 3
	jmp k_sbrx_1
	subi r18, 0xf8
k_sbrx_1:
	sbrc r24, 4
	lds r19, kscratch
	sbrs r24, 5
	sts kscratch, r24
	sbrc r24, 6
	subi r18, 0xc0
	sbrs r24, 7
	subi r18, 0x80
	mov r24, r18
	ret

; SBIC and SBIS on a bit that is clear: PINB (I/O 0x03), with no pins
; driven (and QEMU keeps none of the I/O registers it does not model, so
; a register the program writes would not do)
	.globl k_sbix
k_sbix:
	clr r18
	sbic 0x03, 2
	inc r18
	sbic 0x03, 7
	jmp k_sbix_1
	subi r18, 0xfe
k_sbix_1:
	sbis 0x03, 5
	call inc18
	sbis 0x03, 0
	subi r18, 0xf8
	mov r24, r18
	ret

; SBIC and SBIS on a bit that is set: TIFR1's OCF1A (I/O 0x16, bit 1)
	.globl k_sbit
k_sbit:
	clr r18
	sbis 0x16, 1
	inc r18
	sbis 0x16, 1
	jmp k_sbit_1
	subi r18, 0xfe
k_sbit_1:
	sbic 0x16, 1
	call inc18
	sbic 0x16, 1
	subi r18, 0xf8
	mov r24, r18
	ret

; BST bit 3 of r24 and BLD into bits 6 and 0 of r22: r24 result, r25 SREG
	.globl k_bstbld
k_bstbld:
	out 0x3f, r20
	bst r24, 3
	bld r22, 6
	bst r24, 7
	bld r22, 0
	in r25, 0x3f
	mov r24, r22
	out 0x3f, r1
	ret

; every BRBS and BRBC with SREG r24: r25:r24, the branches not taken
	.macro KBR n
	out 0x3f, r20
	brbs \n, 1f
	ori r18, (1 << \n)
1:	out 0x3f, r20
	brbc \n, 2f
	ori r19, (1 << \n)
2:
	.endm
	.globl k_branches
k_branches:
	mov r20, r24
	ldi r18, 0
	ldi r19, 0
	KBR 0
	KBR 1
	KBR 2
	KBR 3
	KBR 4
	KBR 5
	KBR 6
	KBR 7
	out 0x3f, r1
	mov r24, r18
	mov r25, r19
	ret

; ---- memory -------------------------------------------------------------------

; LD -X twice and X+ from buf + 8: r22 r23 r24 the bytes, r25 X's low byte after
	.globl k_ldx
k_ldx:
	ldi r26, lo8(kbuf + 8)
	ldi r27, hi8(kbuf + 8)
	ld r22, -X
	ld r23, -X
	ld r24, X+
	mov r25, r26
	ldi r18, lo8(kbuf)
	sub r25, r18
	ret

; ST Y+ twice and -Y into buf + 4 (Y is callee-saved): r24 Y's offset after
	.globl k_sty
k_sty:
	push r28
	push r29
	ldi r28, lo8(kbuf + 4)
	ldi r29, hi8(kbuf + 4)
	ldi r18, 0xa5
	ldi r19, 0x3c
	st Y+, r18
	st Y+, r18
	st -Y, r19
	mov r24, r28
	ldi r18, lo8(kbuf)
	sub r24, r18
	pop r29
	pop r28
	ret

; LDD Z+15, STD Z+14, LD Z+ and LD -Z from buf: r22 r23 r24, r25 Z's offset
	.globl k_lddz
k_lddz:
	ldi r30, lo8(kbuf)
	ldi r31, hi8(kbuf)
	ldd r22, Z+15
	std Z+14, r22
	ld r23, Z+
	ld r24, -Z
	mov r25, r30
	ldi r18, lo8(kbuf)
	sub r25, r18
	ret

; LPM Rd, Z+ twice, then LPM (r0) where Z is: r22 r23 r24, r25 Z's advance
	.globl k_lpm
k_lpm:
	ldi r30, lo8(ktable)
	ldi r31, hi8(ktable)
	lpm r22, Z+
	lpm r23, Z+
	lpm
	mov r24, r0
	mov r25, r30
	ldi r18, lo8(ktable)
	sub r25, r18
	ret
ktable:
	.byte 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88

; PUSH and POP: r22 SP's fall, r23 its return, r24 the byte popped
	.globl k_pushpop
k_pushpop:
	in r18, 0x3d
	push r24
	in r19, 0x3d
	pop r25
	in r20, 0x3d
	mov r22, r18
	sub r22, r19
	mov r23, r20
	sub r23, r18
	mov r24, r25
	ret

; MOVW, and the register file read through the data space (Z = 30, r30)
	.globl k_movw
k_movw:
	movw r18, r24
	ldi r30, 30
	ldi r31, 0
	ld r22, Z
	mov r23, r19
	mov r24, r18
	ret

; ---- Timer/Counter1's compare-match A interrupt, and SEI/CLI --------------------
	.globl __vector_11
__vector_11:
	push r24
	in r24, 0x3f
	push r24
	lds r24, ticks
	inc r24
	sts ticks, r24
	pop r24
	out 0x3f, r24
	pop r24
	reti

	.globl k_sei
k_sei:
	sei
	ret

	.globl k_cli
k_cli:
	cli
	ret
