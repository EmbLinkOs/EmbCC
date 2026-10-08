| ColdFire assembly called from C and calling C (tests/golden/coldfire-asm.sh).
| Arguments are on the stack (4(%sp) on entry), results in %d0; %d2-%d7 and
| %a2-%a6 are the caller's. Every branch here is written without a size, so
| the assembler's relaxation picks its forms, and the code a wrong branch
| would fall into changes the result.
#define BIG 1000

	.text
	.global asm_sum
	.type asm_sum, @function
asm_sum:				| int asm_sum(const int *p, int n)
	move.l 4(%sp),%a0
	move.l 8(%sp),%d1
	moveq #0,%d0
	tst.l %d1
	beq 2f
1:	add.l (%a0)+,%d0
	subq.l #1,%d1
	bne 1b
2:	rts
	.size asm_sum, .-asm_sum

	.align 4
	.global asm_call_c
asm_call_c:				| c_helper(x) + c_helper2(x)
	link %fp,#0
	move.l %d2,-(%sp)
	move.l 8(%fp),-(%sp)
	jsr c_helper			| absolute: R_68K_32
	move.l %d0,%d2
	bsr c_helper2			| elsewhere: jsr, R_68K_32
	addq.l #4,%sp
	add.l %d2,%d0
	move.l (%sp)+,%d2
	unlk %fp
	rts

	.global asm_tail
asm_tail:				| a tail call into C (jmp)
	bra c_helper

	.global asm_relax
asm_relax:				| x + 1 + 2 + 3 + 4
	move.l 4(%sp),%d0
	addq.l #1,%d0
	bra 1f
	add.l #BIG,%d0
1:	addq.l #2,%d0
	bra 2f
	add.l #BIG,%d0
	.space 200
2:	addq.l #3,%d0
	cmp.l #100,%d0
	blt 3f
	add.l #BIG,%d0
	.space 20000
3:	addq.l #4,%d0
	rts

	.global asm_alu
asm_alu:				| see model.c
	lea (-8,%sp),%sp
	movem.l %d2-%d3,(%sp)
	move.l 12(%sp),%d0
	move.l 16(%sp),%d1
	move.l %d0,%d2
	add.l %d1,%d2
	move.l %d2,%d3
	add.l %d3,%d2
	add.l %d3,%d2
	sub.l %d1,%d2
	move.l %d0,%d3
	and.l %d1,%d3
	sub.l %d3,%d2
	move.l %d0,%d3
	or.l %d1,%d3
	add.l %d3,%d2
	move.l %d0,%d3
	eor.l %d1,%d3
	add.l %d3,%d2
	move.l %d0,%d3
	asr.l #2,%d3
	add.l %d3,%d2
	move.l %d0,%d3
	lsl.l #3,%d3
	add.l %d3,%d2
	move.l %d0,%d3
	neg.l %d3
	moveq #28,%d1
	lsr.l %d1,%d3
	add.l %d3,%d2
	move.l %d2,%d0
	movem.l (%sp),%d2-%d3
	lea (8,%sp),%sp
	rts

	.global asm_bits
asm_bits:				| *p = 0x0f -> 0x8a; returns bit 1
	move.l 4(%sp),%a0
	bclr #0,(%a0)
	bset #7,(%a0)
	bchg #2,(%a0)
	btst #1,(%a0)
	sne %d0
	extb.l %d0
	neg.l %d0
	rts

	.global asm_table
asm_table:				| a jump through a table of labels
	move.l 4(%sp),%d0
	lea .Ltbl,%a0
	move.l (%a0,%d0.l*4),%a1
	jmp (%a1)
.Lt0:	moveq #10,%d0
	rts
.Lt1:	moveq #20,%d0
	rts
.Lt2:	moveq #30,%d0
	rts

	.global asm_var_bump
asm_var_bump:
	move.l asm_var,%d0
	addq.l #1,%d0
	move.l %d0,asm_var
	rts

	.global asm_div
asm_div:				| a / b * 1000 + a % b * 10 + (unsigned)-a / b
	lea (-8,%sp),%sp
	movem.l %d2-%d3,(%sp)
	move.l 12(%sp),%d0
	move.l 16(%sp),%d1
	move.l %d0,%d2
	divs.l %d1,%d2
	move.l #1000,%d3
	muls.l %d3,%d2
	rems.l %d1,%d3:%d0
	muls.w #10,%d3
	add.l %d3,%d2
	neg.l %d0
	divu.l %d1,%d0
	add.l %d2,%d0
	movem.l (%sp),%d2-%d3
	lea (8,%sp),%sp
	rts

	.global asm_swap
asm_swap:				| the halves exchanged, then the low byte
	move.l 4(%sp),%d0		| sign-extended into the low word's place
	swap %d0
	move.l %d0,%d1
	ext.w %d1
	add.l %d1,%d0
	rts

	.global asm_link
asm_link:				| x + 5 through a frame slot
	link %fp,#-8
	move.l 8(%fp),-4(%fp)
	move.l -4(%fp),%d0
	addq.l #5,%d0
	unlk %fp
	rts

	.global asm_ipl
asm_ipl:				| the interrupt mask in %sr
	move.w %sr,%d0
	lsr.l #8,%d0
	and.l #7,%d0
	rts

	.global asm_trash
asm_trash:				| every caller-saved register changed
	move.l #0x5a5a5a00,%d0
	move.l #0x5a5a5a01,%d1
	move.l #0x5a5a5a08,%a0
	move.l #0x5a5a5a09,%a1
	rts

	.global asm_trash_saved
asm_trash_saved:			| %d2-%d7, %a2-%a5 (a template says so)
	move.l #0x6b6b6b02,%d2
	move.l #0x6b6b6b03,%d3
	move.l #0x6b6b6b04,%d4
	move.l #0x6b6b6b05,%d5
	move.l #0x6b6b6b06,%d6
	move.l #0x6b6b6b07,%d7
	move.l #0x6b6b6b0a,%a2
	move.l #0x6b6b6b0b,%a3
	move.l #0x6b6b6b0c,%a4
	move.l #0x6b6b6b0d,%a5
	rts

	.global trap_handler
trap_handler:				| a trap vector: count, and return
	addq.l #1,trap_count
	rte

	.data
	.align 4
	.global asm_var
asm_var:
	.long 7
	.global trap_count
trap_count:
	.long 0
.Ltbl:
	.long .Lt0, .Lt1, .Lt2
	.word 0x1234
