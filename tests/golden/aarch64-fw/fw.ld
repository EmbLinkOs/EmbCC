ENTRY(_start)
SECTIONS { . = 0x40080000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(16); . = . + 0x10000; __stack_top = .; }
