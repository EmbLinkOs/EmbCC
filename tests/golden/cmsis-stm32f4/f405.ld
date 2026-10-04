/* STM32F405RGTx, in the shape STM32CubeMX generates */
ENTRY(Reset_Handler)
_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;
MEMORY
{
  CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
  RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
}
SECTIONS
{
  .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) . = ALIGN(4); } >FLASH
  .text : { . = ALIGN(4); *(.text) *(.text*) KEEP (*(.init)) KEEP (*(.fini)) . = ALIGN(4); _etext = .; } >FLASH
  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH
  .init_array : { PROVIDE_HIDDEN (__init_array_start = .); KEEP (*(SORT(.init_array.*))) KEEP (*(.init_array*)) PROVIDE_HIDDEN (__init_array_end = .); } >FLASH
  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data) *(.data*) . = ALIGN(4); _edata = .; } >RAM AT> FLASH
  _siccmram = LOADADDR(.ccmram);
  .ccmram : { . = ALIGN(4); _sccmram = .; *(.ccmram) *(.ccmram*) . = ALIGN(4); _eccmram = .; } >CCMRAM AT> FLASH
  . = ALIGN(4);
  .bss : { _sbss = .; __bss_start__ = _sbss; *(.bss) *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; __bss_end__ = _ebss; } >RAM
  ._user_heap_stack : { . = ALIGN(8); PROVIDE ( end = . ); PROVIDE ( _end = . ); . = . + _Min_Heap_Size; . = . + _Min_Stack_Size; . = ALIGN(8); } >RAM
  /DISCARD/ : { libc.a ( * ) libm.a ( * ) libgcc.a ( * ) }
}
