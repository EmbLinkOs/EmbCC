@ A Cortex-M4F context switch in the shape every RTOS port has: PendSV
@ saves the outgoing task's callee-saved registers (and, when the task
@ used the FPU, s16-s31) on its own stack, asks C for the next task, and
@ restores that one's. SVC starts the first task.
    .syntax unified
    .thumb

    .global PendSV_Handler
    .type PendSV_Handler, %function
    .thumb_func
PendSV_Handler:
    mrs     r0, psp
    isb
    ldr     r3, cur_ptr             @ a literal this file defines
    ldr     r2, [r3]
    tst     lr, #0x10               @ an FP frame? (EXC_RETURN bit 4 clear)
    it      eq
    vstmdbeq r0!, {s16-s31}
    stmdb   r0!, {r4-r11, lr}
    str     r0, [r2]
    stmdb   sp!, {r0, r3}
    mov     r0, #0x50
    msr     basepri, r0
    dsb
    isb
    bl      switch_context
    mov     r0, #0
    msr     basepri, r0
    ldmia   sp!, {r0, r3}
    ldr     r1, [r3]
    ldr     r0, [r1]
    ldmia   r0!, {r4-r11, lr}
    tst     lr, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}
    msr     psp, r0
    isb
    bx      lr
    .size PendSV_Handler, .-PendSV_Handler
    .align  2
cur_ptr:
    .word   current                 @ defined in C: an external reference

    .global SVC_Handler
    .thumb_func
SVC_Handler:
    ldr     r3, =current            @ the movw/movt form of the same address
    ldr     r1, [r3]
    ldr     r0, [r1]
    ldmia   r0!, {r4-r11, lr}
    msr     psp, r0
    isb
    mov     r0, #0
    msr     basepri, r0
    bx      lr

    .global start_first
    .thumb_func
start_first:
    movw    r0, #:lower16:vtable
    movt    r0, #:upper16:vtable
    ldr     r1, =0xE000ED08         @ SCB->VTOR
    str     r0, [r1]
    dsb
    isb
    cpsie   i
    svc     #0
    b       never_here              @ an external branch target
