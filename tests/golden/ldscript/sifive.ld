OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
  rom (rx)  : ORIGIN = 0x80000000, LENGTH = 1M
  ram (rwx) : ORIGIN = 0x80100000, LENGTH = 1M
}

REGION_ALIAS("REGION_TEXT", rom);
REGION_ALIAS("REGION_DATA", ram);

SECTIONS
{
  .text : {
    KEEP(*start*.o(.text))
    *(.text .text.*)
  } > REGION_TEXT

  .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) } > REGION_TEXT

  .data : ALIGN(8) {
    _sdata = .;
    *(.data .data.*)
    *(.sdata .sdata.*)
    . = ALIGN(8);
    _edata = .;
  } > REGION_DATA AT > REGION_TEXT
  _sidata = LOADADDR(.data);

  .bss (NOLOAD) : ALIGN(8) {
    _sbss = .;
    *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
    . = ALIGN(8);
    _ebss = .;
  } > REGION_DATA

  .stack (NOLOAD) : ALIGN(16) {
    . += 4K;
    _stack_top = .;
  } > REGION_DATA

  ASSERT(_stack_top <= ORIGIN(ram) + LENGTH(ram), "no room for the stack")
}
