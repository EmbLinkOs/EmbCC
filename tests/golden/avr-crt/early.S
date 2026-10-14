; tests/golden/avr-crt.sh: a step of the startup sequence, in .init3,
; between the stack's set-up (.init2) and the copy of .data (.init4). No
; ret: the sections run one into the next. (avr-libc's documentation
; writes this as a naked C function in .init3; EmbCC places a naked
; function's body in .text, so it is a .S file here.)
#include <avr/io.h>
	.section .init3,"ax",@progbits
	ldi	r24, 0x5a
	sts	early_flag, r24
