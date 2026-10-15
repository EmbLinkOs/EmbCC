@ EmbCC's boot2 for the RP2040: the XIP set up for 03h serial reads, the
@ SDK's boot2_generic_03h's settings, which any SPI flash answers
    .syntax unified
    .cpu cortex-m0plus
    .thumb
    push {lr}
    ldr r3, =0x18000000          @ XIP_SSI_BASE
    movs r1, #0
    str r1, [r3, #8]             @ SSIENR: off, to set it up
    movs r1, #4
    str r1, [r3, #0x14]          @ BAUDR: SCK at clk_sys / 4
    ldr r1, =0x001f0300          @ CTRLR0: 32-bit frames, EEPROM read, standard SPI
    str r1, [r3, #0]
    ldr r1, =0x03000218          @ SPI_CTRLR0: XIP_CMD 03h, an 8-bit instruction, a 24-bit address
    ldr r0, =0x180000f4
    str r1, [r0]
    movs r1, #0
    str r1, [r3, #4]             @ CTRLR1: one frame
    movs r1, #1
    str r1, [r3, #8]             @ SSIENR: on
    pop {r0}                     @ the bootrom's lr is 0; a caller's returns there
    cmp r0, #0
    beq 1f
    bx r0
1:  ldr r0, =0x10000100          @ the vector table, after boot2
    ldr r1, =0xe000ed08          @ VTOR
    str r0, [r1]
    ldmia r0, {r0, r1}
    msr msp, r0
    bx r1
    .ltorg
