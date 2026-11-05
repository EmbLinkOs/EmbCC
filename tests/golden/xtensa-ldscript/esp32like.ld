/* An ESP32-like layout on QEMU's sim machine: code and its literals in one region (IRAM),
 * read-only and writable data in another (DRAM); .data stored after the code and copied by
 * the harness's _start. */
ENTRY(_reset)
MEMORY
{
  IRAM (rx)  : ORIGIN = 0x60010000, LENGTH = 1M
  DRAM (rw)  : ORIGIN = 0x60400000, LENGTH = 2M
}
SECTIONS
{
  .text : {
    KEEP(*(.text.reset))
    *(.text .text.*)
  } > IRAM
  .rodata : {
    *(.rodata .rodata.*)
    . = ALIGN(4);
    __init_array_start = .;
    KEEP(*(.init_array .init_array.*))
    __init_array_end = .;
    __ctors_start = .;
    KEEP(*(.ctors))
    __ctors_end = .;
  } > DRAM
  .data : ALIGN(4) {
    __data_start = .;
    *(.data .data.*)
    . = ALIGN(4);
    __data_end = .;
  } > DRAM AT> IRAM
  __data_load = LOADADDR(.data);
  .bss (NOLOAD) : ALIGN(4) {
    __bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > DRAM
  __stack_top = ORIGIN(DRAM) + LENGTH(DRAM);   /* the region's top */
}
