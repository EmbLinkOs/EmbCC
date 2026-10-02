@ Calls an EmbCC-compiled function with known values in every register
@ AAPCS32 says the callee must preserve (r4-r11, and sp), and returns a
@ bitmask of the ones that came back different: bit n for rn, bit 13 for sp.
@ Written in assembly so no compiler decides which registers hold what.
@
@   unsigned probe(unsigned (*f)(unsigned, unsigned, unsigned, unsigned),
@                  unsigned a, unsigned *result);
    .syntax unified
    .thumb
    .text
    .global probe
    .type probe, %function
    .thumb_func
probe:
    push    {r4-r11, lr}
    sub     sp, sp, #4              @ nine words pushed: keep sp 8-aligned
    ldr     r3, =saved
    str     r2, [r3, #4]            @ where the result goes
    str     sp, [r3]                @ sp before the call
    mov     r12, r0
    mov     r0, r1
    movs    r1, #3
    movs    r2, #5
    movs    r3, #7
    ldr     r4, =0xa4a4a4a4
    ldr     r5, =0xa5a5a5a5
    ldr     r6, =0xa6a6a6a6
    ldr     r7, =0xa7a7a7a7
    ldr     r8, =0xa8a8a8a8
    ldr     r9, =0xa9a9a9a9
    ldr     r10, =0xaaaaaaaa
    ldr     r11, =0xabababab
    blx     r12
    ldr     r1, =saved
    ldr     r2, [r1, #4]
    str     r0, [r2]                @ the function's own result
    movs    r0, #0
    ldr     r2, [r1]
    cmp     sp, r2
    it      ne
    orrne   r0, r0, #0x2000
    ldr     r1, =0xa4a4a4a4
    cmp     r4, r1
    it      ne
    orrne   r0, r0, #0x10
    ldr     r1, =0xa5a5a5a5
    cmp     r5, r1
    it      ne
    orrne   r0, r0, #0x20
    ldr     r1, =0xa6a6a6a6
    cmp     r6, r1
    it      ne
    orrne   r0, r0, #0x40
    ldr     r1, =0xa7a7a7a7
    cmp     r7, r1
    it      ne
    orrne   r0, r0, #0x80
    ldr     r1, =0xa8a8a8a8
    cmp     r8, r1
    it      ne
    orrne   r0, r0, #0x100
    ldr     r1, =0xa9a9a9a9
    cmp     r9, r1
    it      ne
    orrne   r0, r0, #0x200
    ldr     r1, =0xaaaaaaaa
    cmp     r10, r1
    it      ne
    orrne   r0, r0, #0x400
    ldr     r1, =0xabababab
    cmp     r11, r1
    it      ne
    orrne   r0, r0, #0x800
    add     sp, sp, #4
    pop     {r4-r11, pc}
    .ltorg

    .bss
    .align  2
saved:
    .space  8
