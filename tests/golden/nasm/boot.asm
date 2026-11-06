; A boot sector that goes from real mode to long mode: 16-, 32- and
; 64-bit code in one flat image (-f bin), as a hobby OS's first stage is.
[BITS 16]
[ORG 0x7C00]

%ifndef LOAD_SECTORS
%define LOAD_SECTORS 4
%endif
%define CODE32 0x08
%define DATA32 0x10
%define CODE64 0x18

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl
    sti
    mov si, msg
    call print
.load:
    mov ah, 0x02
    mov al, LOAD_SECTORS
    mov cx, 0x0002
    xor dh, dh
    mov dl, [boot_drive]
    mov bx, 0x7E00
    int 0x13
    jc .load
    in al, 0x92
    or al, 2
    out 0x92, al
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE32:pmode

print:
    pusha
.next:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .next
.done:
    popa
    ret

[BITS 32]
pmode:
    mov ax, DATA32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    mov eax, cr4
    or eax, 1 << 5                  ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, 1 << 8                  ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    jmp CODE64:lmode

[BITS 64]
lmode:
    mov rsp, 0x90000
    mov rax, 0x1F201F201F201F20
    mov rdi, 0xB8000
    mov rcx, 500
    rep stosq
    mov rsi, msg
    movzx eax, byte [rsi]
.halt:
    hlt
    jmp .halt

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF           ; 32-bit code
    dq 0x00CF92000000FFFF           ; 32-bit data
    dq 0x00AF9A000000FFFF           ; 64-bit code
gdt_desc:
    dw $ - gdt - 1
    dd gdt

msg db `Booting\r\n`, 0
boot_drive db 0x80

times 510 - ($ - $$) db 0
dw 0xAA55
