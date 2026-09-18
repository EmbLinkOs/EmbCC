/* Test-harness image layout: a Multiboot kernel at 1 MiB.
 *
 * The Multiboot header must fall in the first 8 KiB of the FILE, so it is the
 * first thing placed and link.sh keeps the page size small. Heap and stack
 * are carved out beyond the image, exactly as in the aarch64 harness.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : {
        KEEP(*(.multiboot))
        *(.text.start)
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;

    . = ALIGN(16);
    __heap_start = .;
    . = . + 0x400000;
    __heap_end = .;

    . = ALIGN(16);
    . = . + 0x100000;
    . = ALIGN(16);
    __stack_top = .;

    /DISCARD/ : { *(.eh_frame) *(.note.*) *(.comment) }
}
