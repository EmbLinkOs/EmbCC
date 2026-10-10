/* EmbCC's linker script for the avr5 parts: the layout avr-gcc's default
 * avr5 script gives a program, for a link that names no script of its own
 * (embcc --target=avr -mmcu=PART ... -o fw.elf). tools/build-rt.sh writes
 * one per part, <part>.ld beside crt<part>.o, with the part's memories in
 * place of the @...@ fields (from <avr/io.h>: FLASHEND, RAMSTART, RAMEND,
 * E2END).
 *
 * The linker's view of the two address spaces is avr-ld's: flash from 0,
 * the data space from 0x800000 (SRAM from 0x800000 + RAMSTART), EEPROM
 * from 0x810000.
 *
 * In flash, in this order: the vector table (.vectors, crt<part>.o's),
 * PROGMEM and __flash data (.progmem*), the startup's .init0 to .init9 --
 * contiguous, each falling through to the next -- the code, and .fini9 to
 * .fini0. Then the stored copy of .data, which the startup copies to SRAM.
 * .rodata goes with .data: EmbCC reads read-only data from RAM, as
 * avr-gcc does without PROGMEM. */
OUTPUT_FORMAT("elf32-avr")
OUTPUT_ARCH(avr:5)
ENTRY(__vectors)

MEMORY
{
  text   (rx)   : ORIGIN = 0, LENGTH = @FLASH@
  data   (rw!x) : ORIGIN = 0x800000 + @RAMSTART@, LENGTH = @RAM@
  eeprom (rw!x) : ORIGIN = 0x810000, LENGTH = @EEPROM@
}

SECTIONS
{
  .text :
  {
    KEEP(*(.vectors))
    *(.progmem.gcc*)
    . = ALIGN(2);
    *(.progmem*)
    . = ALIGN(2);
    *(.lowtext*)
    KEEP(*(.init0))
    KEEP(*(.init1))
    KEEP(*(.init2))
    KEEP(*(.init3))
    KEEP(*(.init4))
    KEEP(*(.init5))
    KEEP(*(.init6))
    KEEP(*(.init7))
    KEEP(*(.init8))
    KEEP(*(.init9))
    *(.text)
    . = ALIGN(2);
    *(.text.*)
    . = ALIGN(2);
    KEEP(*(.fini9))
    KEEP(*(.fini8))
    KEEP(*(.fini7))
    KEEP(*(.fini6))
    KEEP(*(.fini5))
    KEEP(*(.fini4))
    KEEP(*(.fini3))
    KEEP(*(.fini2))
    KEEP(*(.fini1))
    KEEP(*(.fini0))
    _etext = .;
  } > text

  .data :
  {
    PROVIDE(__data_start = .);
    *(.data)
    *(.data.*)
    *(.rodata)
    *(.rodata.*)
    . = ALIGN(2);
    PROVIDE(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE(__init_array_end = .);
    _edata = .;
    PROVIDE(__data_end = .);
  } > data AT> text

  __data_load_start = LOADADDR(.data);
  __data_load_end = __data_load_start + SIZEOF(.data);

  .bss (NOLOAD) :
  {
    PROVIDE(__bss_start = .);
    *(.bss)
    *(.bss.*)
    *(COMMON)
    PROVIDE(__bss_end = .);
  } > data

  .noinit (NOLOAD) :
  {
    PROVIDE(__noinit_start = .);
    *(.noinit*)
    PROVIDE(__noinit_end = .);
    _end = .;
    PROVIDE(__heap_start = .);
  } > data

  .eeprom :
  {
    KEEP(*(.eeprom*))
    __eeprom_end = .;
  } > eeprom
}
