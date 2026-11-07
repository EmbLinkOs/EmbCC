	.text
	nop
	.balign 8, 0x90
	nop
	.balign 8, 0xcc
	nop
	.p2align 4, 0x90
	nop
	.p2align 3
	nop
	.p2align 4, 0x90, 3
	ret
	.align 8
	ret
	.data
	.byte 1
	.balign 4, 0xff
	.byte 2
	.balign 8,,2
	.byte 3
	.balignw 8, 0x1234
	.align 4
	.byte 9
