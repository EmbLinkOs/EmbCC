leaq foo(%rip), %rax
movl bar(%rip), %eax
foo: ret
bar: .long 5
.globl g
g: leaq g(%rip), %rax
