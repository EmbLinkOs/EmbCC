/* PowerPC assembly that C calls and that calls C, for
 * tests/golden/ppc-asm.sh: a leaf, a function that saves LR and calls C,
 * a CTR loop on numeric labels, compares into a CR field other than 0, a
 * record form deciding a branch, @ha/@l of a table in .data (in an addi
 * and in a load's offset), the high word of a product, rlwinm/rlwimi,
 * isel, MSR[EE] through wrteei, SPRG2 and SPRG4 through their two
 * numbers, LR read back, a tail call to C, a call through CTR, a
 * lwarx/stwcx. loop, update and byte-reversed loads and a trap that does
 * not fire. main.c prints what each returns. */
#define WORD 4

	.text
	.align 2
	.globl pp_add
	.type pp_add, @function
pp_add:				# a + b
	add 3, 3, 4
	blr
	.size pp_add, .-pp_add

	.globl pp_call
	.type pp_call, @function
pp_call:			# c_twice(x) + 1, with LR saved
	stwu 1, -16(1)
	mflr 0
	stw 0, 20(1)
	bl c_twice
	addi 3, 3, 1
	lwz 0, 20(1)
	mtlr 0
	addi 1, 1, 16
	blr
	.size pp_call, .-pp_call

	.globl pp_sum
	.type pp_sum, @function
pp_sum:				# n + (n - 1) + ... + 1, n > 0, counted by CTR
	mtctr 3
	mr 5, 3
	li 4, 0
1:	add 4, 4, 5
	addi 5, 5, -1
	bdnz 1b
	mr 3, 4
	blr
	.size pp_sum, .-pp_sum

	.globl pp_classify
	.type pp_classify, @function
pp_classify:			# x > 5 ? x + 100 : x, through cr7
	cmpwi cr7, 3, 5
	ble cr7, 2f
	addi 3, 3, 100
2:	blr
	.size pp_classify, .-pp_classify

	.globl pp_rec
	.type pp_rec, @function
pp_rec:				# x >= 10 ? x - 10 : -1, by subic.
	subic. 4, 3, 10
	bge 1f
	li 3, -1
	blr
1:	mr 3, 4
	blr
	.size pp_rec, .-pp_rec

	.globl pp_table
	.type pp_table, @function
pp_table:			# &table
	lis 3, table@ha
	addi 3, 3, table@l
	blr
	.size pp_table, .-pp_table

	.globl pp_load_tab
	.type pp_load_tab, @function
pp_load_tab:			# table[i], @l as the offset of a load
	slwi 3, 3, 2
	lis 4, table@ha
	add 4, 4, 3
	lwz 3, table@l(4)
	blr
	.size pp_load_tab, .-pp_load_tab

	.globl pp_mulhi
	.type pp_mulhi, @function
pp_mulhi:			# the high word of a * b, unsigned
	mulhwu 3, 3, 4
	blr
	.size pp_mulhi, .-pp_mulhi

	.globl pp_div
	.type pp_div, @function
pp_div:
	divwu 3, 3, 4
	blr
	.size pp_div, .-pp_div

	.globl pp_rlw
	.type pp_rlw, @function
pp_rlw:				# byte 1 of a, inserted as byte 2 of b
	rlwinm 5, 3, 16, 24, 31
	rlwimi 4, 5, 8, 16, 23
	mr 3, 4
	blr
	.size pp_rlw, .-pp_rlw

	.globl pp_min
	.type pp_min, @function
pp_min:				# min(a, b), signed, by isel
	cmpw 3, 4
	isel 3, 3, 4, 0
	blr
	.size pp_min, .-pp_min

	.globl pp_ee
	.type pp_ee, @function
pp_ee:				# MSR[EE] after wrteei 1, then the MSR as it was
	mfmsr 5
	wrteei 1
	mfmsr 3
	wrtee 5
	rlwinm 3, 3, 17, 31, 31
	blr
	.size pp_ee, .-pp_ee

	.globl pp_sprg
	.type pp_sprg, @function
pp_sprg:			# v through SPRG2, then v + 1 through SPRG4 (276/260)
	mtspr 274, 3		# SPRG2
	mfsprg 4, 2
	addi 4, 4, 1
	mtspr 276, 4		# SPRG4, written; mfsprg4 reads its copy, 260
	mfspr 3, 260
	blr
	.size pp_sprg, .-pp_sprg

	.globl pp_lr
	.type pp_lr, @function
pp_lr:				# where `bl 1f` left LR, from the function start
	mflr 0
	bl 1f
1:	mflr 3
	mtlr 0
	lis 4, pp_lr@ha
	addi 4, 4, pp_lr@l
	subf 3, 4, 3
	blr
	.size pp_lr, .-pp_lr

	.globl pp_tail
	.type pp_tail, @function
pp_tail:			# c_twice(x + 3), a tail call
	addi 3, 3, 3
	b c_twice
	.size pp_tail, .-pp_tail

	.globl pp_indirect
	.type pp_indirect, @function
pp_indirect:			# f(x), f in r3 and x in r4, through CTR
	stwu 1, -16(1)
	mflr 0
	stw 0, 20(1)
	mtctr 3
	mr 3, 4
	bctrl
	lwz 0, 20(1)
	mtlr 0
	addi 1, 1, 16
	blr
	.size pp_indirect, .-pp_indirect

	.globl pp_cas
	.type pp_cas, @function
pp_cas:				# *p == old ? (*p = new) : 0; returns the old *p
1:	lwarx 6, 0, 3
	cmpw 6, 4
	bne 2f
	stwcx. 5, 0, 3
	bne 1b
2:	mr 3, 6
	blr
	.size pp_cas, .-pp_cas

	.globl pp_bytes
	.type pp_bytes, @function
pp_bytes:			# lbz p[0] + lha p[2..3] + lbzx p[1]
	lbz 4, 0(3)
	lha 5, 2(3)
	li 6, 1
	lbzx 6, 3, 6
	add 4, 4, 5
	add 3, 4, 6
	blr
	.size pp_bytes, .-pp_bytes

	.globl pp_brx
	.type pp_brx, @function
pp_brx:				# *p, byte-reversed
	lwbrx 3, 0, 3
	blr
	.size pp_brx, .-pp_brx

	.globl pp_update
	.type pp_update, @function
pp_update:			# p[1] + p[2] by lwzu, and how far p moved
	mr 6, 3
	lwzu 4, 4(3)
	lwzu 5, 4(3)
	add 4, 4, 5
	subf 3, 6, 3
	add 3, 3, 4
	blr
	.size pp_update, .-pp_update

	.globl pp_notrap
	.type pp_notrap, @function
pp_notrap:			# x, past traps whose conditions do not hold
	twgti 3, 1000
	tweq 3, 4
	twi 4, 3, -1
	blr
	.size pp_notrap, .-pp_notrap

	.data
	.align 3
	.globl table
	.type table, @object
table:	.long counter, 7, pp_add, -WORD
	.size table, .-table
	.globl halves
halves:	.short 0x1234, -2
	.byte 200, -3
	.align 2
	.long 0
