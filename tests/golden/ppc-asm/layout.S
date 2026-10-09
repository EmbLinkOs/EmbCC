/* A file's layout, for tests/golden/ppc-asm.sh: assembled by EmbCC and
 * by clang's integrated assembler, each object linked by embld at the
 * same address with the same C, and the two images compared byte for
 * byte -- labels, .align (an exponent on PowerPC) padded with nops,
 * local branches and calls, a call, a tail branch and @ha/@l of symbols
 * defined elsewhere and here, and data words naming code, data and a
 * numeric label. */
	.text
	.align 2
	.globl lay_a
	.type lay_a, @function
lay_a:
	stwu 1, -16(1)
	mflr 0
	stw 0, 20(1)
	bl ext_fn
	lis 3, ext_data@ha
	lwz 3, ext_data@l(3)
	lis 4, (lay_tab + 4)@ha
	addi 4, 4, (lay_tab + 4)@l
	lis 5, lay_c@h
	ori 5, 5, lay_c@l
	b lay_b
	.size lay_a, .-lay_a

	.align 4
	.globl lay_b
	.type lay_b, @function
lay_b:
	cmpwi cr1, 3, 0
	beq cr1, 1f
	bl lay_a
1:	lwz 0, 20(1)
	mtlr 0
	addi 1, 1, 16
	b ext_tail
	.size lay_b, .-lay_b

	.align 3
lay_words:
	.long lay_a, lay_b, ext_fn, ext_data + 8, 1b
	.short 3
	.byte 1
	.align 2
	.globl lay_c
	.type lay_c, @function
lay_c:
	blr
	.size lay_c, .-lay_c

	.data
	.align 3
	.globl lay_tab
lay_tab:
	.long lay_a, lay_c, ext_data, 0x12345678
	.short -1
	.byte 2, 3
	.align 2
	.long lay_words
