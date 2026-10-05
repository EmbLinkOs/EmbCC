/* mps2-an505 boots secure, from SSRAM1's secure alias at 0x10000000:
 * code from there, the writable image and the stack in the next MiB. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 1M
    RAM   (rwx) : ORIGIN = 0x10100000, LENGTH = 1M
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
ENTRY(Reset_Handler)
SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
    .text : { *(.text*) *(.rodata*) } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH
    _sidata = LOADADDR(.data);
    .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT > FLASH
    .bss : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM
    PROVIDE(_end = .);
}
