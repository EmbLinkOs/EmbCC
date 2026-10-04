/* Every GNU-as feature tests/golden/gas-gnu.sh checks against clang's
 * assembler: the result must be the same object, section by section. */
        .syntax unified
        .cpu    cortex-m4
        .fpu    softvfp
        .thumb

        .equ    STACK_SIZE, 0x400
        .set    HEAP_SIZE, STACK_SIZE * 2 + (1 << 4)
        BUFWORDS = (HEAP_SIZE >> 2) | 3

        .macro  handler name, dflt=Default_Handler
        .weak   \name
        .thumb_set \name, \dflt
        .endm

        .macro  pushall regs:vararg
        push    {\regs}
        .endm

        .section .isr_vector, "a", %progbits
        .align  2
        .globl  vectors
vectors:
        .word   __stack_top
        .word   reset_handler
        .irp    h, nmi_handler, hard_fault, mem_fault
        .word   \h
        .endr
        .rept   3
        .word   0
        .endr
        .word   vectors_end - vectors
vectors_end:
        .size   vectors, . - vectors

        .section .text.reset_handler, "ax", %progbits
        .thumb_func
        .type   reset_handler, %function
        .globl  reset_handler
reset_handler:
        ldr     sp, =__stack_top
        ldr     r0, =_sidata
        ldr     r1, =_sdata
        ldr     r2, =_edata
        ldr     r3, =STACK_SIZE
        ldr     r4, =0x12345678
        ldr     r5, =0xffffff00
        ldr     r6, =_sdata + 8
1:      cmp     r1, r2
        bge     2f
        subs    r2, #4
        ittt    ge
        ldrge   r3, [r0, r2]
        strge   r3, [r1, r2]
        bge     1b
        adds    r1, #4
        b       1b
2:
.if HEAP_SIZE > 0x800
        movs    r0, #1
.else
        movs    r0, #2
.endif
.ifdef  STACK_SIZE
        lsls    r0, #2
.endif
        pushall r4, r5, lr
        ldr     r0, [r1, r2, lsl #2]
        bl      main
        b       far_away
        b       .
        .ltorg

        .pushsection .rodata.table, "a"
table:  .byte   1, 2, 3, BUFWORDS & 0xff
        .short  0x1234, table_end - table
        .4byte  table
table_end:
        .popsection

        .section .text.default_handler, "ax", %progbits
        .thumb_func
        .weak   Default_Handler
Default_Handler:
        b       Default_Handler
        handler nmi_handler
        handler hard_fault
        handler mem_fault, Default_Handler
        .space  2048
far_away:
        nop
        b.n     far_away

        .data
        .fill   2, 2, 0xbf00

        .section .bss.buf, "aw", %nobits
        .align  3
buf:    .space  BUFWORDS * 4
        .end
this line is after .end and never assembled
