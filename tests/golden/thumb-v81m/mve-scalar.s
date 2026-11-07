	.syntax unified
	.thumb
	vmrs r3, p0
	vmsr p0, r2
	vmrs r1, vpr
	vmsr vpr, r4
	vmrs r0, fpcxtns
	vmsr fpcxts, r1
	vmrs r0, fpscr_nzcvqc
	vmsr fpscr_nzcvqc, r0
	lsll r0, r1, r2
	lsll r0, r1, #3
	asrl r2, r3, r4
	asrl r2, r3, #31
	lsrl r4, r5, #1
	uqshll r0, r1, #5
	urshrl r0, r1, #5
	srshrl r0, r1, #32
	sqshll r0, r1, #1
	uqrshll r0, r1, #48, r2
	sqrshrl r0, r1, #64, r3
	uqshl r0, #3
	urshr r1, #4
	srshr r2, #32
	sqshl r3, #1
	uqrshl r4, r5
	sqrshr r6, r7
	lsll lr, r1, r5
	lsll lr, r3, #8
