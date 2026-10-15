/* The exec corpus on EmbSim's Pico: boot2 first, the vector table at
 * 0x10000100; 64 KiB of its SRAM, as the micro:bit's run has, so the two
 * have the same stack and heap (embsim-pico.sh's core check; corpus-boot.c
 * is the startup). */
ENTRY(Reset_Handler)
_estack = ORIGIN(RAM) + LENGTH(RAM);

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 0x200000
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 0x10000
}

SECTIONS
{
  .boot2 : { KEEP(*(.boot2)) } >FLASH
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
