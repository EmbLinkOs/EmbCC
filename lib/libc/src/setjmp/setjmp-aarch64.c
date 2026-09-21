/* setjmp/longjmp for aarch64 (AAPCS64).
 *
 * The same form as the x86-64 file, and the reason is sharper here: EmbCC
 * has no aarch64 assembler at all, so `.long` with the encoding written out
 * is the only way this routine exists on this target. See the sibling file
 * for why that is a design choice rather than a workaround.
 *
 * AAPCS64 calls x19-x28 callee-saved, and also the LOW 64 BITS of v8-v15 --
 * the one place this differs from x86-64, where every vector register is
 * caller-saved. Omitting them is the classic aarch64 setjmp bug: it shows
 * up only when the compiler happens to keep a double in v8 across the
 * setjmp, which depends on optimisation level.
 *
 *   jmp_buf: [0..9]=x19-x28 [10]=x29 [11]=x30 [12]=sp [13..20]=d8-d15
 */
#ifdef __aarch64__

__asm__(
".globl setjmp\n"
"setjmp:\n"
"    .long 0xa9005013   # stp  x19, x20, [x0, #0]\n"
"    .long 0xa9015815   # stp  x21, x22, [x0, #16]\n"
"    .long 0xa9026017   # stp  x23, x24, [x0, #32]\n"
"    .long 0xa9036819   # stp  x25, x26, [x0, #48]\n"
"    .long 0xa904701b   # stp  x27, x28, [x0, #64]\n"
"    .long 0xa905781d   # stp  x29, x30, [x0, #80]\n"
/* sp is not encodable as a source in stp, so it goes through x1 -- a
 * caller-saved register, free to use here. No adjustment: unlike x86-64
 * the return address is in x30, not on the stack, so sp is already the
 * value the caller will see. */
"    .long 0x910003e1   # mov  x1, sp\n"
"    .long 0xf9003001   # str  x1, [x0, #96]\n"
"    .long 0x6d06a408   # stp  d8, d9, [x0, #104]\n"
"    .long 0x6d07ac0a   # stp  d10, d11, [x0, #120]\n"
"    .long 0x6d08b40c   # stp  d12, d13, [x0, #136]\n"
"    .long 0x6d09bc0e   # stp  d14, d15, [x0, #152]\n"
"    .long 0x52800000   # mov  w0, #0\n"
"    .long 0xd65f03c0   # ret\n"

".globl longjmp\n"
"longjmp:\n"
"    .long 0xa9405013   # ldp  x19, x20, [x0, #0]\n"
"    .long 0xa9415815   # ldp  x21, x22, [x0, #16]\n"
"    .long 0xa9426017   # ldp  x23, x24, [x0, #32]\n"
"    .long 0xa9436819   # ldp  x25, x26, [x0, #48]\n"
"    .long 0xa944701b   # ldp  x27, x28, [x0, #64]\n"
"    .long 0xa945781d   # ldp  x29, x30, [x0, #80]\n"
"    .long 0x6d46a408   # ldp  d8, d9, [x0, #104]\n"
"    .long 0x6d47ac0a   # ldp  d10, d11, [x0, #120]\n"
"    .long 0x6d48b40c   # ldp  d12, d13, [x0, #136]\n"
"    .long 0x6d49bc0e   # ldp  d14, d15, [x0, #152]\n"
/* sp last among the restores, and x0's buffer is read before it moves:
 * everything below the new sp is dead memory the moment sp changes. */
"    .long 0xf9403002   # ldr  x2, [x0, #96]\n"
"    .long 0x9100005f   # mov  sp, x2\n"
"    .long 0x2a0103e0   # mov  w0, w1\n"
/* longjmp(env, 0) returns 1 from setjmp (§7.13.2.1p3). */
"    .long 0x7100001f   # cmp  w0, #0\n"
"    .long 0x54000041   # b.ne .+8\n"
"    .long 0x52800020   # mov  w0, #1\n"
"    .long 0xd65f03c0   # ret\n"   /* x30 came from the buffer */
);

#endif
