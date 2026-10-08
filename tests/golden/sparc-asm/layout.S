/* A file's layout, for tests/golden/sparc-asm.sh: assembled by EmbCC and
 * by clang's integrated assembler, each object linked by embld at the
 * same address with the same C, and the two images compared byte for
 * byte -- labels, .align (a byte count on SPARC) padded with nops, local
 * branches and calls, a call and %hi/%lo and `set` of symbols defined
 * elsewhere and here, and data words naming code, data and a numeric
 * label. */
	.text
	.align 4
	.global lay_a
	.type lay_a, #function
lay_a:
	save %sp, -96, %sp
	call ext_fn
	 nop
	sethi %hi(ext_data), %o0
	ld [%o0 + %lo(ext_data)], %o0
	set lay_tab + 4, %o1
	sethi %hi(lay_c), %o2
	or %o2, %lo(lay_c), %o2
	ba lay_b
	 nop
	.size lay_a, .-lay_a

	.align 16
	.global lay_b
	.type lay_b, #function
lay_b:
	cmp %o0, 0
	be,a 1f
	 mov 1, %o0
	call lay_a
	 nop
1:	call ext_tail
	 restore
	.size lay_b, .-lay_b

	.align 8
lay_words:
	.word lay_a, lay_b, ext_fn, ext_data + 8, 1b
	.half 3
	.byte 1
	.align 4
	.global lay_c
	.type lay_c, #function
lay_c:
	retl
	 nop
	.size lay_c, .-lay_c

	.data
	.align 8
	.global lay_tab
lay_tab:
	.word lay_a, lay_c, ext_data, 0x12345678
	.half -1
	.byte 2, 3
	.align 4
	.word lay_words
