/* Bare-metal entry for the x86-64 test harness.
 *
 * QEMU's -kernel loads a Multiboot image and enters it in 32-bit protected
 * mode with paging off, no usable stack, and SSE disabled. A test compiled
 * by embcc is 64-bit SysV code that newlib expects to run with SSE on, so
 * this climbs the rest of the way: identity page tables, PAE, EFER.LME,
 * paging, a 64-bit GDT, the far jump into long mode, then SSE and .bss.
 *
 * The object is 64-bit ELF; link.sh re-labels the linked image as ELF32
 * (objcopy -O elf32-i386), which is what QEMU's Multiboot loader accepts.
 * Everything sits below 4 GiB, so the 32-bit view of the addresses is exact.
 */
    .set MB_MAGIC, 0x1BADB002
    .set MB_FLAGS, 0x00000003          /* page-align modules, give memory info */

    .section .multiboot, "a"
    .align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long -(MB_MAGIC + MB_FLAGS)

    .section .text.start, "ax"
    .code32
    .global _start
_start:
    cli
    mov     $__stack_top, %esp

    /* Identity-map the low 1 GiB with 2 MiB pages: pml4[0] -> pdpt,
     * pdpt[0] -> pd, pd[i] = i * 2 MiB. The tables live in .data (not
     * .bss) so they are already zero when this runs — .bss is not cleared
     * until long mode. */
    mov     $pdpt, %eax
    or      $0x3, %eax                 /* present | writable */
    mov     %eax, pml4
    mov     $pd, %eax
    or      $0x3, %eax
    mov     %eax, pdpt
    xor     %ecx, %ecx
1:  mov     %ecx, %eax
    shl     $21, %eax
    or      $0x83, %eax                /* present | writable | 2 MiB page */
    mov     %eax, pd(,%ecx,8)
    inc     %ecx
    cmp     $512, %ecx
    jne     1b

    /* ... except the FIRST 2 MiB, which is split into 4 KiB pages so
     * that the one at address 0 can be left absent.
     *
     * A harness that identity-maps from zero makes a null dereference
     * succeed: `*(int *)0 = 1` writes, reads back, and the test passes.
     * That is not a hypothetical -- it means no test in tests/exec or
     * tests/cxx could ever detect one, on the target where most of them
     * run. With this page gone the store faults, and with no IDT the
     * fault escalates to a triple fault, which -no-reboot turns into
     * QEMU exiting and run.sh reports as 125, "the guest crashed".
     *
     * Only the first page goes: the image is linked at 1 MiB and
     * everything else in the low 2 MiB stays mapped exactly as before. */
    mov     $pt0, %eax
    or      $0x3, %eax                 /* a TABLE pointer, not a 2 MiB page */
    mov     %eax, pd
    mov     $1, %ecx                   /* entry 0 stays zero: NULL is absent */
3:  mov     %ecx, %eax
    shl     $12, %eax
    or      $0x3, %eax                 /* present | writable */
    mov     %eax, pt0(,%ecx,8)
    inc     %ecx
    cmp     $512, %ecx
    jne     3b

    mov     $pml4, %eax
    mov     %eax, %cr3
    mov     %cr4, %eax
    or      $(1 << 5), %eax            /* CR4.PAE */
    mov     %eax, %cr4
    mov     $0xC0000080, %ecx          /* IA32_EFER */
    rdmsr
    or      $(1 << 8), %eax            /* EFER.LME */
    wrmsr
    mov     %cr0, %eax
    or      $(1 << 31), %eax           /* CR0.PG: now in compatibility mode */
    mov     %eax, %cr0

    lgdt    gdt_ptr
    ljmp    $0x08, $long_mode

    .code64
long_mode:
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     %ax, %fs
    mov     %ax, %gs
    mov     $__stack_top, %rsp

    /* SSE: newlib's printf and every float test use it. CR0.EM off,
     * CR0.MP on, CR4.OSFXSR and CR4.OSXMMEXCPT on — the x86 counterpart of
     * the CPACR_EL1.FPEN step in the aarch64 harness. */
    mov     %cr0, %rax
    and     $~(1 << 2), %rax
    or      $(1 << 1), %rax
    mov     %rax, %cr0
    mov     %cr4, %rax
    or      $((1 << 9) | (1 << 10)), %rax
    mov     %rax, %cr4
    fninit

    mov     $__bss_start, %rdi
    mov     $__bss_end, %rcx
    sub     %rdi, %rcx
    xor     %eax, %eax
    rep stosb

    and     $-16, %rsp
    call    __harness_main             /* ../crt.c: constructors, main, exit */
2:  hlt
    jmp     2b

    .section .rodata
    .align 8
gdt:
    .quad   0
    .quad   0x00AF9A000000FFFF         /* 0x08: 64-bit code, ring 0 */
    .quad   0x00CF92000000FFFF         /* 0x10: data */
gdt_end:
gdt_ptr:
    .word   gdt_end - gdt - 1
    .long   gdt

    .section .data
    .align 4096
pml4:   .fill 4096, 1, 0
pdpt:   .fill 4096, 1, 0
pd:     .fill 4096, 1, 0
pt0:    .fill 4096, 1, 0               /* the first 2 MiB, 4 KiB at a time */
