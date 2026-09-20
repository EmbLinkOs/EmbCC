/* setjmp/longjmp for x86-64 (System V).
 *
 * Written as bytes with the disassembly beside each one. That is not
 * squeamishness about assemblers: EmbCC's file-scope assembler encodes a
 * handful of mnemonics, and the moment a routine needs more than those the
 * honest options are to grow a full assembler inside the compiler or to
 * place the instructions directly. `embcc -S` already chose the second for
 * the same reason -- the bytes are what runs, and nothing downstream gets
 * to re-decide them. `tests/golden/libc.sh` disassembles these back out of
 * the built library and checks them against the comments, so the two
 * cannot drift.
 *
 * What must be saved is exactly what the ABI calls callee-saved, because a
 * longjmp lands in a function that believes its callees preserved them:
 * rbx, rbp, r12-r15, plus the stack pointer and the return address. The
 * SSE registers are all caller-saved on this ABI, so there are none here.
 *
 *   jmp_buf: [0]=rbx [1]=rbp [2]=r12 [3]=r13 [4]=r14 [5]=r15
 *            [6]=rsp-after-return [7]=return address
 *
 * The bytes are the ones x86_64-elf-as produces for the comments beside
 * them, byte for byte, and the golden test proves it by assembling the
 * comments -- which is how the d8/d9 encoding in the aarch64 sibling was
 * caught being wrong.
 */
#ifdef __x86_64__

__asm__(
".globl setjmp\n"
"setjmp:\n"
"    .byte 0x48,0x89,0x1f            # mov  %rbx, (%rdi)\n"
"    .byte 0x48,0x89,0x6f,0x08       # mov  %rbp, 0x08(%rdi)\n"
"    .byte 0x4c,0x89,0x67,0x10       # mov  %r12, 0x10(%rdi)\n"
"    .byte 0x4c,0x89,0x6f,0x18       # mov  %r13, 0x18(%rdi)\n"
"    .byte 0x4c,0x89,0x77,0x20       # mov  %r14, 0x20(%rdi)\n"
"    .byte 0x4c,0x89,0x7f,0x28       # mov  %r15, 0x28(%rdi)\n"
/* The stack pointer to record is the one this function's caller will see,
 * which is rsp+8: the return address is still on the stack here and the
 * `ret` about to run pops it. Recording rsp itself would resume with the
 * stack eight bytes too low, and every local of the calling frame off. */
"    .byte 0x48,0x8d,0x44,0x24,0x08  # lea  0x8(%rsp), %rax\n"
"    .byte 0x48,0x89,0x47,0x30       # mov  %rax, 0x30(%rdi)\n"
"    .byte 0x48,0x8b,0x04,0x24       # mov  (%rsp), %rax\n"
"    .byte 0x48,0x89,0x47,0x38       # mov  %rax, 0x38(%rdi)\n"
"    .byte 0x31,0xc0                 # xor  %eax, %eax\n"
"    .byte 0xc3                      # ret\n"

".globl longjmp\n"
"longjmp:\n"
"    .byte 0x48,0x8b,0x1f            # mov  (%rdi), %rbx\n"
"    .byte 0x48,0x8b,0x6f,0x08       # mov  0x08(%rdi), %rbp\n"
"    .byte 0x4c,0x8b,0x67,0x10       # mov  0x10(%rdi), %r12\n"
"    .byte 0x4c,0x8b,0x6f,0x18       # mov  0x18(%rdi), %r13\n"
"    .byte 0x4c,0x8b,0x77,0x20       # mov  0x20(%rdi), %r14\n"
"    .byte 0x4c,0x8b,0x7f,0x28       # mov  0x28(%rdi), %r15\n"
/* rdx before rsp: once rsp moves, anything below it is fair game for an
 * interrupt or a signal handler to overwrite, so nothing may be read from
 * the old frame afterwards. rdi points at the caller's jmp_buf, which is
 * in the frame being RETURNED to, so it stays valid. */
"    .byte 0x48,0x8b,0x57,0x38       # mov  0x38(%rdi), %rdx\n"
"    .byte 0x48,0x8b,0x67,0x30       # mov  0x30(%rdi), %rsp\n"
"    .byte 0x89,0xf0                 # mov  %esi, %eax\n"
/* §7.13.2.1p3: longjmp(env, 0) must make setjmp return 1, not 0 --
 * otherwise the caller cannot tell the jump from the original call. */
"    .byte 0x85,0xc0                 # test %eax, %eax\n"
"    .byte 0x75,0x05                 # jne  .+7\n"
"    .byte 0xb8,0x01,0x00,0x00,0x00  # mov  $1, %eax\n"
"    .byte 0xff,0xe2                 # jmp  *%rdx\n"
);

#endif
