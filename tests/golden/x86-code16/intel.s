.intel_syntax noprefix
mov eax, DWORD PTR [rbx+rcx*4+8]
lea rax, [rip+foo]
mov rax, OFFSET foo
movzx eax, BYTE PTR [rdi]
vaddps zmm0{k1}{z}, zmm1, zmm2
call foo
push QWORD PTR fs:0x28
.att_syntax
movl %eax, %ebx
foo: ret
