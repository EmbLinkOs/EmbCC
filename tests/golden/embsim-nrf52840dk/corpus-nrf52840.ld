/* The exec corpus on the nRF52840 DK's memory -- 1 MiB of flash at 0,
 * 256 KiB of RAM at 0x20000000 -- on EmbSim's nrf52840dk and on QEMU's
 * mps2-an386 (a Cortex-M4 with FPv4-SP, memory at both addresses): the
 * core check of embsim-nrf52840dk.sh; embsim-pico/corpus-boot.c is the
 * startup. */
ENTRY(Reset_Handler)
_estack = ORIGIN(RAM) + LENGTH(RAM);

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 0x100000
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 0x40000
}

SECTIONS
{
  .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) . = ALIGN(4); } >FLASH
  .text : { . = ALIGN(4); *(.text) *(.text*) . = ALIGN(4); } >FLASH
  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH
  .ARM.exidx : { *(.ARM.exidx*) } >FLASH
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .);
                  KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*))
                  PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data) *(.data*) . = ALIGN(4);
            _edata = .; } >RAM AT> FLASH
  .bss : { . = ALIGN(4); _sbss = .; *(.bss) *(.bss*) *(COMMON) . = ALIGN(4);
           _ebss = .; } >RAM
  PROVIDE(end = _ebss);
  PROVIDE(_end = _ebss);
}
