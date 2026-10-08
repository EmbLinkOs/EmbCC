@ Operands every core refuses (sp/pc, RdLo = RdHi, bit fields past the
@ word, ldrd's offsets, pairs and writeback bases, the wrong operand
@ count), then forms only some cores have. arm-asm-more.sh adds this to
@ the vocabulary and compares what EmbCC refuses at each Cortex-M level
@ with what llvm-mc refuses for that core.
	mla r0, r1, r2, sp
	mla pc, r1, r2, r3
	mls r0, r1, r2
	smull r0, r0, r1, r2
	umull r3, r3, r4, r5
	smlal r0, sp, r1, r2
	umaal r4, r4, r1, r2
	umlal r0, r1, r2
	rev16 sp, r1
	revsh r0, pc
	rev16 r0, r1, r2
	rrx r0, sp
	rrx pc, r1
	bfi r0, r1, #0, #33
	bfi r0, r1, #31, #2
	bfi r0, r1, #32, #1
	bfi r0, r1, #4, #0
	bfc r0, #31, #2
	bfc sp, #1, #2
	bfc r0, r1, #1, #2
	sbfx r0, r1, #1, #32
	ubfx r0, r1, #31, #2
	sbfx r0, pc, #1, #2
	ubfx r0, r1, #3
	ldrd r0, r0, [r2]
	ldrd r0, r1, [r0, #8]!
	ldrd r0, r1, [r1], #8
	strd r0, r1, [r0, #-8]!
	strd r2, r3, [r3], #8
	ldrd r0, r1, [r2, #1024]
	ldrd r0, r1, [r2, #6]
	ldrd r0, r1, [r2], #-1024
	ldrd sp, r1, [r2]
	ldrd r0, pc, [r2]
	strd r0, sp, [r2]
	ldrd r0, r1
	ldrexd r0, r1, [r2]
	strexd r3, r0, r1, [r2]
	mla r0, r1, r2, r3
	umaal r0, r1, r2, r3
	smull r8, r9, r10, r11
	rev16 r0, r1
	revsh r7, r0
	revsh r8, r1
	rev16 r1, r9
	rrxs r0, r1
	bfc r0, #1, #2
	sbfx r0, r1, #0, #1
	ldrd r0, r1, [r2, #-1020]
	ldrd lr, r0, [r2]
	strd r0, r1, [sp], #-8
	clrex
	ldrexb r0, [r1]
	strexh r0, r2, [r1]
	ldrex r0, [r1]
