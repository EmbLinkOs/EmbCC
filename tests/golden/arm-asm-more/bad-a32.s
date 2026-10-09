@ ARM state (armv7a-none-eabi): operands A32 refuses -- pc, an odd or
@ non-consecutive ldrd pair, A32's offsets and bounds -- and forms it has
@ that Thumb state does not (sp as an operand, asr #32, the doubleword
@ exclusives, rotations on the plain extends). arm-asm-more.sh compares
@ what EmbCC refuses with what llvm-mc refuses for armv7a.
	sadd16 r0, r1, pc
	uqasx pc, r1, r2
	qadd r0, pc, r1
	smlalbb pc, r0, r1, r2
	smlald r3, r3, r1, r2
	ssat r0, #8, pc
	ssat r0, #8, r1, asr #33
	ssat r0, #0, r1
	usat r0, #32, r1
	usat16 r0, #16, r1
	pkhbt r0, r1, r2, lsl #32
	pkhtb r0, r1, r2, asr #0
	sxtab r0, r1, r2, ror #4
	sxtb r0, pc
	mla r0, r1, r2, pc
	smull r0, r0, r1, r2
	bfi r0, r1, #31, #2
	bfc r0, #0, #33
	ubfx r0, pc, #0, #4
	ldrd r1, r2, [r3]
	ldrd r0, r2, [r3]
	ldrd lr, pc, [r3]
	ldrd r0, r1, [r2, #256]
	ldrd r0, r1, [r2, #-256]
	ldrd r0, r1, [r0, #8]!
	strd r0, r1, [r1], #8
	ldrexd r1, r2, [r3]
	ldrexd r0, r2, [r3]
	ldrexd r0, r1, [r2, #8]
	strexd r0, r2, r4, [r5]
	sxtb r0, r1, ror #8
	uxtah r0, sp, r1, ror #24
	ssat r0, #8, r1, asr #32
	ldrd r0, r1, [pc, #8]
	ldrd r12, sp, [r2], #-255
	umaal r0, r1, r2, r3
	smulwt sp, sp, sp
	strexd r4, r0, r1, [r2]
	sadd16eq r0, r1, r2
	smlabbne r0, r1, r2, r3
	mlsgt r0, r1, r2, r3
	ldrdlt r0, r1, [r2]
