/* A script in the shape of avr-gcc's default avr5 script, for
 * tests/golden/avr-crt.sh: the data region's origin is a default the
 * driver is expected to move (avr-gcc passes -Tdata 0x800100 for the
 * ATmega328P, and so does EmbCC's driver under -mmcu=); each startup
 * section is named twice, once to place it and once in KEEP() to keep it
 * under --gc-sections; constructors by SORT(*)(.ctors); .bss and
 * .noinit after .data, placed by address. Written for this test. */
OUTPUT_FORMAT("elf32-avr","elf32-avr","elf32-avr")
OUTPUT_ARCH(avr:5)
__TEXT_REGION_LENGTH__ = DEFINED(__TEXT_REGION_LENGTH__) ? __TEXT_REGION_LENGTH__ : 128K;
__DATA_REGION_ORIGIN__ = DEFINED(__DATA_REGION_ORIGIN__) ? __DATA_REGION_ORIGIN__ : 0x800060;
__DATA_REGION_LENGTH__ = DEFINED(__DATA_REGION_LENGTH__) ? __DATA_REGION_LENGTH__ : 0xffa0;
MEMORY
{
  text   (rx)   : ORIGIN = 0, LENGTH = __TEXT_REGION_LENGTH__
  data   (rw!x) : ORIGIN = __DATA_REGION_ORIGIN__, LENGTH = __DATA_REGION_LENGTH__
  eeprom (rw!x) : ORIGIN = 0x810000, LENGTH = 64K
}
SECTIONS
{
  .text :
  {
    *(.vectors)
    KEEP(*(.vectors))
    *(.progmem.gcc*)
    . = ALIGN(2);
    *(.progmem*)
    . = ALIGN(2);
     __ctors_start = . ;
     *(.ctors)
     __ctors_end = . ;
    KEEP(SORT(*)(.ctors))
    *(.init0)
    KEEP (*(.init0))
    *(.init1)
    KEEP (*(.init1))
    *(.init2)
    KEEP (*(.init2))
    *(.init3)
    KEEP (*(.init3))
    *(.init4)
    KEEP (*(.init4))
    *(.init5)
    KEEP (*(.init5))
    *(.init6)
    KEEP (*(.init6))
    *(.init7)
    KEEP (*(.init7))
    *(.init8)
    KEEP (*(.init8))
    *(.init9)
    KEEP (*(.init9))
    *(.text)
    . = ALIGN(2);
    *(.text.*)
    . = ALIGN(2);
    *(.fini9)
    KEEP (*(.fini9))
    *(.fini0)
    KEEP (*(.fini0))
    _etext = . ;
  } > text
  .data :
  {
    PROVIDE (__data_start = .) ;
    *(.data)
    *(.data*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(2);
    PROVIDE (__init_array_start = .) ;
    KEEP (*(.init_array))
    PROVIDE (__init_array_end = .) ;
    _edata = . ;
    PROVIDE (__data_end = .) ;
  } > data AT> text
  .bss ADDR(.data) + SIZEOF (.data) : AT (ADDR (.bss))
  {
    PROVIDE (__bss_start = .) ;
    *(.bss)
    *(.bss*)
    *(COMMON)
    PROVIDE (__bss_end = .) ;
  } > data
  __data_load_start = LOADADDR(.data);
  __data_load_end = __data_load_start + SIZEOF(.data);
  .noinit ADDR(.bss) + SIZEOF (.bss) : AT (ADDR (.noinit))
  {
    PROVIDE (__noinit_start = .) ;
    *(.noinit*)
    PROVIDE (__noinit_end = .) ;
    _end = . ;
  } > data
  .eeprom :
  {
    KEEP(*(.eeprom*))
  } > eeprom
}
