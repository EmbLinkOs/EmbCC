    .section .text.start, "ax"
    .global _start
_start:
    ldr     x0, =__stack_top
    mov     sp, x0
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    bl      cmain
1:  b       1b
