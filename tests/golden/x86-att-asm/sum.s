	.text
	.globl	asm_sum
	.type	asm_sum, @function
# long asm_sum(const long *p, long n): the sum, a loop
asm_sum:
	xorl	%eax, %eax
	testq	%rsi, %rsi
	je	2f
1:
	addq	(%rdi), %rax
	addq	$8, %rdi
	decq	%rsi
	jnz	1b
2:
	addq	bias(%rip), %rax
	ret
	.size	asm_sum, .-asm_sum
	.globl	asm_call
asm_call:			# int asm_call(int x): helper(x) + 1, through the PLT
	pushq	%rbx
	call	helper
	incl	%eax
	popq	%rbx
	ret
	.data
bias:	.quad	100
