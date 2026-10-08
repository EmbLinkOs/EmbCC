@ Operands every core refuses (sp/pc, bounds and shifts out of range, RdLo
@ = RdHi, a rotation not 8/16/24, the wrong operand count), then forms
@ only some cores have: the plain extends and ssat/usat. thumb-dsp.sh adds
@ this to the DSP vocabulary and compares what EmbCC refuses at each level
@ with what llvm-mc refuses for that core.
	sadd16 r0, r1, sp
	sadd16 pc, r1, r2
	qadd r0, sp, r2
	smlad r0, r1, r2, pc
	smlad r0, r1, r2, sp
	smuad r0, r1
	smlald r3, r3, r1, r2
	smlalbb r4, r4, r1, r2
	ssat r0, #0, r1
	ssat r0, #33, r1
	usat r0, #32, r1
	ssat r0, #8, r1, asr #0
	ssat r0, #8, r1, lsl #32
	ssat16 r0, #0, r1
	ssat16 r0, #17, r1
	usat16 r0, #16, r1
	ssat16 r0, #4, r1, lsl #1
	pkhbt r0, r1, r2, lsl #32
	pkhbt r0, r1, r2, asr #3
	pkhtb r0, r1, r2, asr #0
	pkhtb r0, r1, r2, lsl #3
	sxtb16 r0, r1, ror #4
	sxtab r0, r1, r2, ror #32
	sxtb16 r0, sp
	sxtah r0, pc, r2
	sel r0, r1, r2, r3
	usada8 r0, r1, r2
	smmls r0, r1, r2
	sxtb r0, r1, ror #8
	sxtb r8, r1
	uxth r0, r9
	ssat r0, #8, r1
	usat r0, #8, r1, asr #3
