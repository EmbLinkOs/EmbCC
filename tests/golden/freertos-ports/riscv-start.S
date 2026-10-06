/* QEMU's virt board: the reset entry for FreeRTOS's RISC-V port. A stack,
 * machine-mode traps to the port's handler (direct mode), then C. */

    .section .text.start, "ax"
    .globl _start
_start:
    la      sp, _estack
    la      t0, freertos_risc_v_trap_handler
    csrw    mtvec, t0
    call    c_start
1:  j       1b
