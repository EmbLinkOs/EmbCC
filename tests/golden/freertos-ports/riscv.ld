/* QEMU virt with -m 8: RAM at 0x80000000, where -kernel loads the image. */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 8M
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
ENTRY(_start)
SECTIONS
{
    .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) *(.srodata*) } > RAM
    _sidata = LOADADDR(.data);
    .data : { _sdata = .; *(.data*) *(.sdata*) _edata = .; } > RAM
    .bss : { _sbss = .; *(.bss*) *(.sbss*) *(COMMON) _ebss = .; } > RAM
    PROVIDE(_end = .);
}
