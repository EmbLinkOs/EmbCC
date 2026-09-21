/* The layout of a static Linux image built by EmbCC.
 *
 * This exists because of one requirement that is easy to miss and fatal
 * to get wrong: THE PROGRAM HEADERS MUST BE INSIDE THE FIRST LOADABLE
 * SEGMENT.
 *
 * The kernel tells a program where its own headers are through the
 * auxiliary vector, AT_PHDR -- and it can only do that if some PT_LOAD
 * covers the file offset they sit at. When none does, Linux passes
 * AT_PHDR = 0 and the program has no way to find them. Nothing fails at
 * link time and nothing fails at load time; what fails is the first
 * thing that needed them.
 *
 * Here that is thread-local storage. PT_TLS is the template every
 * thread's block is copied from, and ./tls.c finds it by walking the
 * program headers. With AT_PHDR zero it finds nothing, sets no thread
 * pointer, and the first `__thread` access reads from near address
 * zero. That is exactly what happened: aarch64-elf-ld's default script
 * starts .text at 0x400000 with the headers before it and outside every
 * segment, while x86_64-elf-ld's happens to include them -- so the same
 * program worked on one architecture and took SIGSEGV on the other.
 *
 * `SIZEOF_HEADERS` is the whole fix: it reserves room for the ELF
 * header and the program headers at the start of the first segment, so
 * they are mapped and AT_PHDR is real.
 *
 * A distribution's ld does this by default. These bare-metal cross
 * linkers do not, and there is no aarch64 Linux emulation installed to
 * borrow one from -- so the layout is written down here rather than
 * inherited from whichever toolchain is present, which also makes it
 * the same on both architectures.
 */
ENTRY(_start)

SECTIONS
{
    /* Not page-aligned on purpose: the headers go first, and the code
     * follows them inside the same segment. */
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text .text.*)
    }
    .rodata : {
        *(.rodata .rodata.*)
    }
    /* Unwind tables, for C++ exceptions. Read-only, so they belong with
     * the text segment rather than the data one. */
    .eh_frame : {
        *(.eh_frame)
    }
    .gcc_except_table : {
        *(.gcc_except_table .gcc_except_table.*)
    }

    /* A page boundary, because what follows is writable and a segment's
     * permissions are granted a page at a time. CONSTANT(MAXPAGESIZE)
     * rather than a number: it is 4K on x86-64 and 64K on aarch64, and
     * aligning to the smaller of the two on the larger target leaves ld
     * unable to split the segments at all -- it merges them into one
     * RWX mapping, which both works and is exactly what a program
     * should not have. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* ---- the thread-block template ------------------------------------
     *
     * These two carry SHF_TLS, so ld gathers them into PT_TLS rather
     * than making them part of what every thread shares. They come
     * first in the writable segment so the template is contiguous.
     *
     * .tbss is NOBITS and occupies no file space; its size still
     * counts toward the template, which is why PT_TLS's memsz exceeds
     * its filesz whenever a program has an uninitialised __thread. */
    .tdata : {
        *(.tdata .tdata.*)
    }
    .tbss : {
        *(.tbss .tbss.*)
        *(.tcommon)
    }

    /* Static constructors and destructors. The brackets are what
     * ../start.c walks; ld's built-in script PROVIDEs these, and a
     * custom script has to define them itself or every constructor
     * silently stops running. */
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    }
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }

    .data : {
        *(.data .data.*)
    }
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /* Debug information, kept out of the loadable image. */
    .debug_abbrev  0 : { *(.debug_abbrev) }
    .debug_info    0 : { *(.debug_info) }
    .debug_line    0 : { *(.debug_line) }

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
    }
}
