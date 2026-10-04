/* A RISC-V hart starts with every register zero: something has to set sp
 * before the first C function, and that is this file. Placed first by
 * the script (`KEEP(*start*.o(.text))`). */
    .text
    .globl _start
_start:
    la sp, _stack_top
    call reset_c
1:  j 1b
