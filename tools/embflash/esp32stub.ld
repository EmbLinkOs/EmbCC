/* embflash's flasher stub (esp32stub.c), in RAM the ROM's loader leaves
 * alone: code in IRAM, its data and buffers in DRAM below the ROM's own
 * (0x3FFE0000). .bss is not loaded: the stub zeroes it. */
ENTRY(stub_main)
MEMORY
{
  IRAM (rx) : ORIGIN = 0x40090000, LENGTH = 32K
  DRAM (rw) : ORIGIN = 0x3FFB0000, LENGTH = 64K
}
SECTIONS
{
  .text : { KEEP(*(.text.stub_main)) *(.literal .text .literal.* .text.*) } > IRAM
  .rodata : { *(.rodata .rodata.*) } > DRAM
  .data : ALIGN(4) { *(.data .data.*) } > DRAM
  .bss (NOLOAD) : ALIGN(4) { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > DRAM
}
