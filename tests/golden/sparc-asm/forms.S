/* SPARC assembly that C calls and that calls C, for
 * tests/golden/sparc-asm.sh: leaf functions that answer in their delay
 * slots, a windowed function that calls C, a counted loop on numeric
 * labels, annulled branches taken and not, %hi/%lo of a table in .data
 * (in an `or` and in a load's offset), umul's high word through %y, a
 * divide that reads %y, `set`, the PSR's interrupt level, a tail call
 * from a leaf, a call through a register, casa, swap, ldd and the narrow
 * loads. main.c prints what each returns. */
#define WORD 4

	.text
	.align 4
	.global sp_add
	.type sp_add, #function
sp_add:				! a + b, in the slot of retl
	retl
	 add %o0, %o1, %o0
	.size sp_add, .-sp_add

	.global sp_call
	.type sp_call, #function
sp_call:			! c_twice(x) + 1, in a window of its own
	save %sp, -96, %sp
	call c_twice
	 mov %i0, %o0
	ret
	 restore %o0, 1, %o0
	.size sp_call, .-sp_call

	.global sp_sum
	.type sp_sum, #function
sp_sum:				! n + (n - 1) + ... + 1, n > 0
	clr %o1
1:	add %o1, %o0, %o1
	subcc %o0, 1, %o0
	bne 1b
	 nop
	retl
	 mov %o1, %o0
	.size sp_sum, .-sp_sum

	.global sp_annul
	.type sp_annul, #function
sp_annul:			! x > 5 ? x + 100 : x -- the slot runs only when taken
	cmp %o0, 5
	bg,a 2f
	 add %o0, 100, %o0
2:	retl
	 nop
	.size sp_annul, .-sp_annul

	.global sp_ba_annul
	.type sp_ba_annul, #function
sp_ba_annul:			! x: `ba,a` annuls its slot although it is taken
	ba,a 3f
	 add %o0, 1, %o0
	nop
3:	retl
	 nop
	.size sp_ba_annul, .-sp_ba_annul

	.global sp_table
	.type sp_table, #function
sp_table:			! &table
	sethi %hi(table), %o0
	retl
	 or %o0, %lo(table), %o0
	.size sp_table, .-sp_table

	.global sp_load_tab
	.type sp_load_tab, #function
sp_load_tab:			! table[i], %lo() as the offset of a load
	sll %o0, 2, %o0
	sethi %hi(table), %o1
	add %o1, %o0, %o1
	retl
	 ld [%o1 + %lo(table)], %o0
	.size sp_load_tab, .-sp_load_tab

	.global sp_umulhi
	.type sp_umulhi, #function
sp_umulhi:			! the high word of a * b
	umul %o0, %o1, %o0
	retl
	 rd %y, %o0
	.size sp_umulhi, .-sp_umulhi

	.global sp_div
	.type sp_div, #function
sp_div:				! a / b, unsigned, with %y the high word (0)
	wr %g0, %g0, %y
	nop
	nop
	nop
	retl
	 udiv %o0, %o1, %o0
	.size sp_div, .-sp_div

	.global sp_set
	.type sp_set, #function
sp_set:
	set 0x12345678, %o0
	set 4095, %o1
	set 0x400000, %o2
	sub %o0, %o1, %o0
	retl
	 sub %o0, %o2, %o0
	.size sp_set, .-sp_set

	.global sp_pil
	.type sp_pil, #function
sp_pil:				! the PIL field of the PSR, which the harness set to 15
	rd %psr, %o0
	srl %o0, 8, %o0
	retl
	 and %o0, 15, %o0
	.size sp_pil, .-sp_pil

	.global sp_tail
	.type sp_tail, #function
sp_tail:			! c_twice(x + 3), a tail call from a leaf
	add %o0, 3, %o0
	mov %o7, %g1
	call c_twice
	 mov %g1, %o7
	.size sp_tail, .-sp_tail

	.global sp_indirect
	.type sp_indirect, #function
sp_indirect:			! f(x), f in %o0 and x in %o1
	save %sp, -96, %sp
	jmpl %i0, %o7
	 mov %i1, %o0
	ret
	 restore %o0, %g0, %o0
	.size sp_indirect, .-sp_indirect

	.global sp_casa
	.type sp_casa, #function
sp_casa:			! *p == old ? (*p = new) : 0; returns the old *p
	casa [%o0] 10, %o1, %o2
	retl
	 mov %o2, %o0
	.size sp_casa, .-sp_casa

	.global sp_swap
	.type sp_swap, #function
sp_swap:			! *p = v, returning the old *p
	swap [%o0], %o1
	retl
	 mov %o1, %o0
	.size sp_swap, .-sp_swap

	.global sp_ldd
	.type sp_ldd, #function
sp_ldd:				! p[0] - p[1], the two words of one ldd
	ldd [%o0], %o2
	retl
	 sub %o2, %o3, %o0
	.size sp_ldd, .-sp_ldd

	.global sp_bytes
	.type sp_bytes, #function
sp_bytes:			! ldub p[0] + ldsb p[1] + ldsh p[2..3]
	ldub [%o0], %o1
	ldsb [%o0 + 1], %o2
	ldsh [%o0 + 2], %o3
	add %o1, %o2, %o1
	retl
	 add %o1, %o3, %o0
	.size sp_bytes, .-sp_bytes

	.global sp_stores
	.type sp_stores, #function
sp_stores:			! p[0..1] = the low half of v, p[2] = its low byte, std
	sth %o1, [%o0]
	stb %o1, [%o0 + 2]
	mov 9, %o2
	mov 10, %o3
	retl
	 std %o2, [%o0 + 8]
	.size sp_stores, .-sp_stores

	.data
	.align 8
	.global table
	.type table, #object
table:	.word counter, 7, sp_add, -WORD
	.size table, .-table
	.global halves
halves:	.half 0x1234, -2
	.byte 200, -3
	.align 4
	.word 0
