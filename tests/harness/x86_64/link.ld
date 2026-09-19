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
    /* C++ exceptions: the unwind tables, which ../crt.c registers with
     * libgcc's unwinder (a bare-metal image has no dl_iterate_phdr), ended
     * by the zero word the unwinder stops at; and the LSDAs */
    .eh_frame : {
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)
    }
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) }
    /* static constructors (C++ globals), run by ../crt.c before main.
     * .init_array runs forward; .ctors — what x86_64-elf gcc still emits —
     * runs BACKWARD, so it is laid out as GNU ld does: plain entries, then
     * the prioritised .ctors.N (N = 65535 - priority) sorted, so the walk
     * from the end runs the lowest priority value first. */
    .ctors : {
        __ctors_start = .;
        KEEP(*(.ctors))
        KEEP(*(SORT(.ctors.*)))
        __ctors_end = .;
    }
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }
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

    /DISCARD/ : { *(.note.*) *(.comment) }
}
