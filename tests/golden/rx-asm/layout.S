; A file's layout, assembled by EmbCC and by GNU as and linked by embld at
; the same address (tests/golden/rx-asm.sh): the images and the symbols
; must be the same -- labels, alignment and its padding, relaxed branches
; within the file, calls and branches to symbols defined elsewhere, data
; words naming symbols, sections.
#define N 3
	.equ COUNT, 12

	.text
	.global _lay_a
_lay_a:
	mov.l #_ext_data, r1
	mov.l #_ext_data+8, r2
	mov.l #lay_local, r3
	bsr _ext_fn
	bsr.w _ext_fn
	bsr.a _ext_fn
	beq _ext_fn
	bne.b _ext_fn
	bgt _ext_fn
	bltu.b _ext_fn
	bra.w _ext_fn
	mov.l #COUNT, r4
	add #N, r4
1:	sub #1, r4
	bne 1b
	bra 2f
	nop
2:	cmp #COUNT, r4
	blt lay_b
	bra _ext_tail
	.balign 4
lay_local:
	rts
	.align 8
	.global _lay_b
lay_b:
_lay_b:
	bra.s 3f
	nop
	nop
3:	beq lay_far
	bgeu lay_far
	bsr lay_far
	bra lay_far
	.space 300, 0x03
	bra 4f
	.space 40000, 0x03
4:	bra _lay_a
	bra lay_b
lay_far:
	rts
	.balign 16
	.global _lay_vec
_lay_vec:
	.long _lay_a, lay_b, _ext_fn, _ext_data + 4
	.word lay_local
	.short 0x1234
	.byte 1, 2, 3
	.ascii "rx"
	.balign 2
	rts

	.section .rodata
	.global _lay_ro
_lay_ro:
	.long lay_far, 0x55aa55aa
	.asciz "layout"

	.data
	.balign 4
	.global _lay_data
_lay_data:
	.long 1, _lay_ro, _ext_data
	.short -1

	.bss
	.balign 8
	.global _lay_bss
_lay_bss:
	.space 24
