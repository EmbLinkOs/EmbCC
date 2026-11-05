/* The ESP32's startup, for a program the ROM boots from 0x1000 (through
 * embflash's boot stub, tools/embflash/esp32boot.c, when it has code or
 * data in flash). In IRAM: it runs before anything is known about the
 * rest. The ROM leaves the windowed ABI's state as its own entry had it;
 * this makes the program's own:
 *
 *   interrupts off (INTENABLE 0): the ROM's sources are the ROM's;
 *   this window the only live one (WINDOWSTART = 1 << WINDOWBASE), so no
 *   overflow ever spills a ROM frame into the new stack;
 *   sp at the top of the program's stack (esp32.ld's __stack_top), below
 *   a 32-byte bottom frame whose save-area pointer is set, as the ABI's
 *   first frame needs;
 *   PS for the windowed ABI: WOE and UM, interrupt level 0;
 *
 * then __embcc_esp32_start (lib/esp32/start.c) by callx8: it is in flash,
 * further than a call8 reaches. It never returns. */
    .section .text.esp32.reset, "ax"
    .global _reset
    .align 4
_reset:
    j       1f
    .align  4
.Lsp:
    .word   __stack_top - 32
.Lps:
    .word   0x00040020
.Lstart:
    .word   __embcc_esp32_start
1:
    movi    a2, 0
    wsr     a2, intenable
    rsr     a3, windowbase
    ssl     a3
    movi    a2, 1
    sll     a2, a2
    wsr     a2, windowstart
    rsync
    l32r    a1, .Lsp
    addi    a2, a1, 32
    addi    a3, a1, -16
    s32i    a2, a3, 4
    l32r    a2, .Lps
    wsr     a2, ps
    rsync
    l32r    a8, .Lstart
    callx8  a8
2:
    j       2b
