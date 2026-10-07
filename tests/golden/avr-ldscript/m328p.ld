/* An ATmega328P in avr-libc's style (avr5.x): flash from 0, data space
 * at 0x800000 in the linker's view, SRAM from 0x100. */
OUTPUT_FORMAT("elf32-avr","elf32-avr","elf32-avr")
OUTPUT_ARCH(avr:5)
ENTRY(__vectors)
MEMORY
{
  text   (rx)   : ORIGIN = 0, LENGTH = 32K
  data   (rw!x) : ORIGIN = 0x800100, LENGTH = 2K
}
SECTIONS
{
  .text :
  {
    *(.vectors)
    KEEP(*(.vectors))
    *(.text)
    *(.text.*)
  } > text
  .data :
  {
    PROVIDE (__data_start = .) ;
    *(.rodata)
    *(.rodata*)
    *(.data)
    *(.data*)
    . = ALIGN(2);
    PROVIDE (__data_end = .) ;
  } > data AT> text
  __data_load = LOADADDR(.data);
  __data_load_start = LOADADDR(.data);
  .bss ADDR(.data) + SIZEOF (.data) (NOLOAD) :
  {
    PROVIDE (__bss_start = .) ;
    *(.bss)
    *(.bss*)
    *(COMMON)
    PROVIDE (__bss_end = .) ;
  } > data
  __heap_start = . ;
}
