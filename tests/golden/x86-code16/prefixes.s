.code32
lock xaddw %ax, %es:(%bx)
lock incl (%esi)
rep movsw
rep movsw %es:(%si), %es:(%di)
.code64
lock xaddw %ax, (%ebx)
lock addl $1, %fs:(%rax)
