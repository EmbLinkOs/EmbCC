@ EmbSim's ROM for the RP2040 (tools/embsim/rp2040.c): the bootrom's vector
@ table, header and table lookup, the hand-over to boot2 as the bootrom
@ makes it, and its _dead. The test assembles this with llvm-mc and holds
@ what the board reads at 0x00-0x33 to it (no relocation: it is at 0).
    .syntax unified
    .cpu cortex-m0plus
    .thumb
rom:
    .word 0x20041f00              @ the bootrom's stack top
    .word reset - rom + 1         @ (at 0: an offset is the address)
    .word dead - rom + 1          @ NMI
    .word dead - rom + 1          @ HardFault
    .byte 'M', 'u', 1, 3          @ magic, version 1, ROM version 3 (B2)
    .hword table - rom, table - rom, lookup - rom + 1, 0
lookup:                           @ rom_table_lookup finds nothing
    movs r0, #0
    bx lr
reset:                            @ enter boot2 in SRAM with lr 0
    ldr r0, =0x20041f01
    movs r1, #0
    mov lr, r1
    bx r0
dead:
    wfi
    b dead
table:                            @ the function and data tables, empty
    .hword 0, 0
    .ltorg
