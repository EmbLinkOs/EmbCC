	.code16
start:
	cli
	xorw %ax, %ax
	movw %ax, %ds
	movw $0x7c00, %sp
	movb %dl, boot_drive
	movw $msg, %si
	call print
	movb boot_drive, %dl
	movb boot_drive, %al
	movl %eax, %es:4(%bx,%si)
	movw (%bp), %ax
	movw 300(%bp,%di), %cx
	pushw $5
	push $0x1234
	pushl $3
	push %es
	pop %ds
	pusha
	popal
	incw %cx
	decl %edx
	jne start
	jmp far1
	lgdt gdt
	movl %cr0, %eax
	orl $1, %eax
	movl %eax, %cr0
	ljmp $0x08, $pm
	loop start
	jcxz start
	retw
	ret $4
	lret
	iret
	iretl
	movsw
	movsl
	stosb
	cwtl
	cbtw
	inb $0x60, %al
	outw %ax, %dx
	int $0x10
	jmp *%ax
	call *(%bx)
	lss (%bx), %sp
	enter $8, $0
	les (%di), %ax
	.code32
pm:	movw $0x10, %ax
	movw %ax, %ds
	movl $0x90000, %esp
	movl 0x1234, %eax
	pushl %eax
	push %eax
	incl %eax
	jmp far1
	call print
	movl %cr4, %eax
	ljmp $0x08, $start
	lgdt gdt
	jmp *%eax
	ret
	.code64
	movq %rax, %rbx
	jmp far1
far1:
	nop
print:	ret
gdt:	.quad 0
msg:	.byte 1
boot_drive: .byte 0x80
