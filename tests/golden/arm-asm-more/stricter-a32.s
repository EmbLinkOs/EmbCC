@ ARM state: UNPREDICTABLE forms llvm-mc takes and EmbCC refuses by name --
@ pc as a register of the multiplies and reversals (smlad with Ra pc is
@ smuad's encoding), a status register that is one of the others.
	smlad r0, r1, r2, pc
	umaal r0, r1, r2, pc
	rev16 r0, pc
	revsh pc, r0
	strexd r0, r0, r1, [r2]
	strexd r2, r0, r1, [r2]
