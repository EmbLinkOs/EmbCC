/* An ESP32 program booted by its ROM from 0x1000, with no second-stage
 * bootloader (embcc --board=esp32-devkit, or --target=xtensa-esp32-elf
 * with this script).
 *
 *   IROM  code, in flash, read through the cache: embflash's boot stub
 *         maps it before the program starts (tools/embflash/esp32boot.c);
 *   DROM  read-only data, in flash the same way (byte loads are fine);
 *   IRAM  the startup, and what must not wait for flash: functions in
 *         .iram1 (IRAM_ATTR) and their literals;
 *   DRAM  .data (a segment the ROM loads), .bss, the heap and the stack.
 *         It ends at 0x3FFE0000, where the ROM keeps its own data while
 *         it loads the image.
 *
 * Literals go before the code that loads them (l32r reaches only
 * backwards, 256 KiB): -mtext-section-literals puts each function's in its
 * own section, which the patterns below keep in order. */
ENTRY(_reset)
MEMORY
{
  IRAM (rx) : ORIGIN = 0x40080000, LENGTH = 128K
  DRAM (rw) : ORIGIN = 0x3FFB0000, LENGTH = 192K
  IROM (rx) : ORIGIN = 0x400D0000, LENGTH = 3264K
  DROM (r)  : ORIGIN = 0x3F400000, LENGTH = 4M
}
SECTIONS
{
  .iram.text : {
    KEEP(*(.text.esp32.reset))
    *(.iram1.literal .iram1 .iram1.*)
  } > IRAM
  .flash.text : {
    *(.literal .text .literal.* .text.*)
  } > IROM
  .flash.rodata : {
    *(.rodata .rodata.*)
    . = ALIGN(4);
    __init_array_start = .;
    KEEP(*(.init_array .init_array.*))
    __init_array_end = .;
  } > DROM
  .data : ALIGN(4) {
    __data_start = .;
    *(.data .data.*)
    . = ALIGN(4);
    __data_end = .;
  } > DRAM
  .bss (NOLOAD) : ALIGN(4) {
    __bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > DRAM
  _end = .;
  __stack_top = ORIGIN(DRAM) + LENGTH(DRAM);
}
