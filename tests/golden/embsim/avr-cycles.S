; avr-cycles.S -- sequences whose cycles the ATmega328P's datasheet gives
; ("Instruction Set Summary", "Interrupt Response Time"), for
; tests/golden/embsim-avr.sh: QEMU has no cycle model, so the datasheet
; is the referee. Every line that takes time says how much, as ";c N"
; (the cycles it takes in this run: a branch taken, a skip skipping, an
; instruction skipped as 0). The test runs this main and an empty one
; (main: ret) and EmbSim's cycles must differ by the sum of the ";c"s.

	.text
	.globl main
main:
	; ---- one cycle each
	ldi r24, 0x5a		;c 1
	ldi r25, 0xa5		;c 1
	add r24, r25		;c 1
	adc r24, r25		;c 1
	sub r24, r25		;c 1
	subi r24, 3		;c 1
	sbc r24, r25		;c 1
	sbci r24, 3		;c 1
	and r24, r25		;c 1
	andi r24, 0x7f		;c 1
	or r24, r25		;c 1
	ori r24, 1		;c 1
	eor r24, r25		;c 1
	com r24			;c 1
	neg r24			;c 1
	inc r24			;c 1
	dec r24			;c 1
	tst r24			;c 1
	clr r24			;c 1
	ser r24			;c 1
	cp r24, r25		;c 1
	cpc r24, r25		;c 1
	cpi r24, 9		;c 1
	mov r18, r24		;c 1
	movw r18, r24		;c 1
	lsl r24			;c 1
	lsr r24			;c 1
	rol r24			;c 1
	ror r24			;c 1
	asr r24			;c 1
	swap r24		;c 1
	bst r24, 3		;c 1
	bld r25, 5		;c 1
	sec			;c 1
	clc			;c 1
	sez			;c 1
	clz			;c 1
	in r18, 0x3f		;c 1
	out 0x3f, r1		;c 1
	nop			;c 1
	wdr			;c 1
	; ---- two
	adiw r24, 1		;c 2
	sbiw r24, 63		;c 2
	mul r24, r25		;c 2
	muls r24, r25		;c 2
	mulsu r18, r19		;c 2
	fmul r18, r19		;c 2
	fmuls r18, r19		;c 2
	fmulsu r18, r19		;c 2
	clr r1			;c 1
	ldi r26, 0x00		;c 1
	ldi r27, 0x02		;c 1
	movw r28, r26		;c 1
	movw r30, r26		;c 1
	st X, r24		;c 2
	st X+, r24		;c 2
	st -X, r24		;c 2
	ld r24, X		;c 2
	ld r24, X+		;c 2
	ld r24, -X		;c 2
	std Y+5, r24		;c 2
	ldd r24, Y+5		;c 2
	st Y+, r24		;c 2
	ld r24, -Y		;c 2
	st Z+, r24		;c 2
	ld r24, -Z		;c 2
	ldd r24, Z+63		;c 2
	lds r24, 0x0210		;c 2
	sts 0x0210, r24		;c 2
	push r24		;c 2
	pop r24			;c 2
	sbi 0x04, 1		;c 2
	cbi 0x04, 1		;c 2
	rjmp cyc_1		;c 2
	nop			;c 0
cyc_1:
	ldi r30, pm_lo8(cyc_2)	;c 1
	ldi r31, pm_hi8(cyc_2)	;c 1
	ijmp			;c 2
	nop			;c 0
cyc_2:
	; ---- three and four
	ldi r30, lo8(cyc_table)	;c 1
	ldi r31, hi8(cyc_table)	;c 1
	lpm			;c 3
	lpm r24, Z		;c 3
	lpm r24, Z+		;c 3
	jmp cyc_3		;c 3
	nop			;c 0
cyc_3:
	rcall cyc_sub		;c 3 (and the ret, 4, below)
	call cyc_sub		;c 4 (and the ret)
	ldi r30, pm_lo8(cyc_sub) ;c 1
	ldi r31, pm_hi8(cyc_sub) ;c 1
	icall			;c 3 (and the ret)
	; ---- branches: taken 2, not 1
	sec			;c 1
	brcs cyc_4		;c 2
	nop			;c 0
cyc_4:
	clc			;c 1
	brcs cyc_5		;c 1
	nop			;c 1
cyc_5:
	; ---- skips: 1 not skipping, 2 over one word, 3 over two
	ldi r24, 7		;c 1
	ldi r25, 7		;c 1
	cpse r24, r25		;c 2
	nop			;c 0
	cpse r24, r25		;c 3
	jmp cyc_6		;c 0
cyc_6:
	cpse r24, r1		;c 1
	nop			;c 1
	sbrc r24, 3		;c 2
	nop			;c 0
	sbrs r24, 0		;c 3
	lds r18, 0x0200		;c 0
	sbrs r24, 3		;c 1
	nop			;c 1
	sbi 0x04, 2		;c 2
	sbis 0x04, 2		;c 3
	call cyc_sub		;c 0
	sbic 0x04, 2		;c 1
	nop			;c 1
	cbi 0x04, 2		;c 2
	; ---- an interrupt: USART0's data register empty (vector 19), taken
	; once UDRIE0 is set with I set; its entry 4, the vector's JMP 3
	ldi r24, 0x20		;c 1
	sei			;c 1
	sts 0xc1, r24		;c 2 (UCSR0B.UDRIE0: the interrupt now)
	;c 4 (the entry: PC pushed, I cleared)
	;c 3 (the vector's jmp)
	;c 2 (__vector_19's sts)
	;c 4 (__vector_19's reti)
	cli			;c 1 (one instruction runs after RETI first)
	ret			; (the empty main's too: not counted)

cyc_sub:
	ret			;c 4 4 4 (the three calls above return through here)

	.globl __vector_19
__vector_19:
	sts 0xc1, r1		; UDRIE0 off (counted above)
	reti			; (counted above)

cyc_table:
	.byte 1, 2, 3, 4
