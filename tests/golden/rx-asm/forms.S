; RX assembly called from C and calling C (tests/golden/rx-asm.sh): the
; same file is assembled by EmbCC and by GNU as, and both objects must
; make main.c print the same lines. Every branch here is written without
; a size, so each object's relaxation picks its forms; the code a wrong
; branch would fall into changes the result.
#define BIG 1000

	.text
	.global _asm_sum
	.type _asm_sum, @function
_asm_sum:				; int asm_sum(const int *p, int n)
	mov.l #0, r3
	cmp #0, r2
	beq 2f
1:	mov.l [r1+], r4
	add r4, r3
	sub #1, r2
	bne 1b
2:	mov.l r3, r1
	rts
	.size _asm_sum, .-_asm_sum

	.align 4
	.global _asm_call_c
_asm_call_c:				; c_helper(x) + x, x kept in r6
	pushm r6-r7
	mov.l r1, r6
	bsr _c_helper
	add r6, r1
	popm r6-r7
	rts

	.global _asm_tail
_asm_tail:				; a tail call into C
	bra _c_helper

	.balign 8
	.global _asm_relax
_asm_relax:				; x + 1 + 2 + 3 + 4
	add #1, r1
	bra 1f
	add #BIG, r1
1:	add #2, r1
	bra 2f
	add #BIG, r1
	.space 200, 0x03
2:	add #3, r1
	cmp #0, r1
	bne 3f
	add #BIG, r1
	.space 300, 0x03
3:	add #4, r1
	cmp #100, r1
	blt 4f
	add #BIG, r1
	.space 200, 0x03
4:	nop ! nop
	rts

	.global _asm_alu
_asm_alu:				; see main.c
	add r1, r2, r3
	mul #3, r3
	sub r2, r3
	mov.l r1, r4
	and r2, r4
	sub r4, r3
	mov.l r1, r4
	or r2, r4
	add r4, r3
	mov.l r1, r4
	xor r2, r4
	add r4, r3
	shar #2, r1, r4
	add r4, r3
	shll #3, r1, r4
	add r4, r3
	neg r1, r4
	shlr #28, r4
	add r4, r3
	mov.l r3, r1
	rts

	.global _asm_bits
_asm_bits:				; *p = 0x0f -> 0x8e; returns bit 1
	bclr #0, [r1].b
	bset #7, [r1].b
	btst #1, [r1].b
	scne.l r2
	mov.l r2, r1
	rts

	.global _asm_table
_asm_table:				; a jump through a table of labels
	mov.l #.Ltbl, r2
	mov.l [r1, r2], r2
	jmp r2
.Lt0:	mov.l #10, r1
	rts
.Lt1:	mov.l #20, r1
	rts
.Lt2:	mov.l #30, r1
	rts

	.global _asm_copy
_asm_copy:				; smovf r3 bytes; the last one copied
	smovf
	sub #1, r1
	movu.b [r1], r1
	rts

	.global _asm_usp_roundtrip
_asm_usp_roundtrip:
	pushc psw
	mvfc usp, r2
	mvtc r1, usp
	mvfc usp, r3
	mvtc r2, usp
	popc psw
	mov.l r3, r1
	rts

	.global _asm_var_bump
_asm_var_bump:
	mov.l #_asm_var, r2
	mov.l [r2], r1
	add #1, r1
	mov.l r1, [r2]
	rts

	.global _asm_div
_asm_div:				; a / b * 1000 + (unsigned)-a / b
	mov.l r1, r3
	div r2, r3
	mul #1000, r3
	neg r1
	divu r2, r1
	add r3, r1
	rts

	.global _asm_long_branch
_asm_long_branch:			; x * 2 + 1, over 33000 bytes
	add r1, r1
	bra 1f
	add #BIG, r1
	.space 33000, 0x03
1:	add #1, r1
	rts

	.global _asm_trash
_asm_trash:				; every caller-saved register changed
	mov.l #0x5a5a5a01, r1
	mov.l #0x5a5a5a02, r2
	mov.l #0x5a5a5a03, r3
	mov.l #0x5a5a5a04, r4
	mov.l #0x5a5a5a05, r5
	mov.l #0x5a5a5a0e, r14
	mov.l #0x5a5a5a0f, r15
	rts

	.global _asm_trash_saved
_asm_trash_saved:			; r6-r12 changed (a template says so)
	mov.l #0x6b6b6b06, r6
	mov.l #0x6b6b6b07, r7
	mov.l #0x6b6b6b08, r8
	mov.l #0x6b6b6b09, r9
	mov.l #0x6b6b6b0a, r10
	mov.l #0x6b6b6b0b, r11
	mov.l #0x6b6b6b0c, r12
	rts

	.data
	.balign 4
	.global _asm_var
_asm_var:
	.long 7
.Ltbl:
	.long .Lt0, .Lt1, .Lt2
