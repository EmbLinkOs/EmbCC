/* limit.ld -- the lm3s6965's memory for tests/golden/embsim-stack.sh,
 * with the stack's limit in the link, as a vendor's script puts it:
 * __stack_limit, 2 KiB below the top of RAM (where the vector table puts
 * the stack). The harness's brackets are here too. */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
SECTIONS
{
    .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
    .data : { __data_start = .; *(.data*) __data_end = .; } > RAM AT > FLASH
    __data_load = LOADADDR(.data);
    .bss : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; } > RAM
    _end = .;
    __init_array_start = .;
    __init_array_end = .;
    __stack_limit = ORIGIN(RAM) + LENGTH(RAM) - 2048;
}
