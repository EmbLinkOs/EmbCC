@ UNPREDICTABLE forms llvm-mc takes and EmbCC refuses by name (in Thumb
@ state): a writeback through pc.
	ldrd r0, r1, [pc, #8]!
	ldrd r0, r1, [pc], #8
