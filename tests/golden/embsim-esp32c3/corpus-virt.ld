/* The exec corpus on QEMU's virt (embsim-esp32c3.sh): everything in its
 * RAM at 0x80000000, the stack at the top of 8 MiB. */
ENTRY(_start)
MEMORY
{
  RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 8M
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS
{
  .text : { KEEP(*(.text.reset_stub)) *(.text) *(.text*) } >RAM
  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) *(.srodata*) } >RAM
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .);
                  KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*))
                  PROVIDE_HIDDEN(__init_array_end = .); } >RAM
  .data : { . = ALIGN(4); _sdata = .; *(.data) *(.data*) *(.sdata*) . = ALIGN(4);
            _edata = .; } >RAM
  _sidata = LOADADDR(.data);
  .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss) *(.bss*) *(.sbss*) *(COMMON)
                    . = ALIGN(4); _ebss = .; } >RAM
  PROVIDE(end = _ebss);
  PROVIDE(_end = _ebss);
}
