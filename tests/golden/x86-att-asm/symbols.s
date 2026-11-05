	.text
	.globl	f
	.type	f, @function
f:
	pushq	%rbp
	movq	%rsp, %rbp
	testl	%edi, %edi
	je	1f
	call	g
	call	g@PLT
	jmp	.Ldone
1:
	movl	counter(%rip), %eax
	leaq	table(%rip), %rsi
	movq	ext@GOTPCREL(%rip), %rax
	addl	$1, counter+4(%rip)
	movl	$counter, %ecx
	movq	$counter, %rdx
	movabsq	$counter, %r8
	jne	2f
	.fill	200, 1, 0x90
2:
	jmp	far
	xorl	%eax, %eax
.Ldone:
	popq	%rbp
	ret
	.size	f, .-f
back:
	decl	%ecx
	jnz	back
	.fill	300, 1, 0x90
	jmp	back
	.data
counter:
	.long	1, 2
table:
	.quad	f
	.quad	ext + 8
	.long	back
