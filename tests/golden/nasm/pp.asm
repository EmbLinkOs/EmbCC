; NASM's preprocessor and directives in an elf64 object: macros with
; parameters, defaults, a greedy last one, %0, %% labels and %rotate;
; %rep with %assign; %if/%elif/%else, %ifidn, %ifnum, %strlen; %xdefine
; against %define; a function-like %define; %include; struc and istruc;
; equ; align and alignb; strings (backquoted, with escapes); a float;
; incbin; global with a type and a size; extern; common; default rel.
%include "defs.inc"

bits 64
default rel

%macro PUSHALL 1-*
%rep %0
    push %1
%rotate 1
%endrep
%endmacro

%macro POPALL 1-*
%rep %0
%rotate -1
    pop %1
%endrep
%endmacro

%macro CLEAR 1-2 0
    mov %1, %2
%endmacro

%macro LOOPN 2+
    mov ecx, %1
%%again:
    %2
    dec ecx
    jnz %%again
%endmacro

%define BASE 10
%xdefine EARLY BASE
%undef BASE
%define BASE 20

section .text
extern helper, other_helper
global entry:function (entry.end - entry)
global table:data 16

entry:
    PUSHALL rbx, rbp, r12
    CLEAR eax
    CLEAR edx, 7
    LOOPN 3, inc eax
    mov eax, EARLY
    mov edx, BASE
    mov esi, MAKE_VERSION(VERSION_MAJOR, VERSION_MINOR)
    lea rdi, [table]
    mov rax, [rel counter]
    mov rbx, [rsp + frame.flags]
    mov ecx, frame_size
    call helper
    call other_helper wrt ..plt
%ifidn BASE, 20
    add eax, 1
%else
    add eax, 2
%endif
%if VERSION_MINOR > 5
    sub eax, 1
%elif VERSION_MINOR == 4
    sub eax, 2
%else
    sub eax, 3
%endif
%ifnum VERSION_MAJOR
    xor ebx, ebx
%endif
%strlen NAMELEN "embcc"
    mov ecx, NAMELEN
    POPALL rbx, rbp, r12
    ret
.end:

align 16
square:
    imul edi, edi
    mov eax, edi
    ret

section .data
counter: dq 0
%assign i 0
squares:
%rep 6
    dd i * i
%assign i i + 1
%endrep
greeting: db `Hello, \x41\t!\n`, 0
padded:   dw 'abc'
half:     dd 1.5
whole:    dq -2.25
blob:     incbin "blob.bin", 2, 4
align 8, db 0
istack:
    istruc frame
        at frame.rip, dq 0x1000
        at frame.ss,  dq 0x10
    iend

section .rodata
table: dq entry, square, counter, squares
limit equ table + 32 - $$

section .bss
alignb 16
buffer: resb 100
words:  resw 3
common shared 64:16
