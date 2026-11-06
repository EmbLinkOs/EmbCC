PHDRS
{
    headers PT_PHDR PHDRS;
    text PT_LOAD FILEHDR PHDRS;
    data PT_LOAD;
    tls PT_TLS;
    note PT_NOTE;
    stack PT_GNU_STACK FLAGS(6);
}
ENTRY(_start)
SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;
    .text   : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) }
    .note.x : { *(.note.x) } :text :note
    . = ALIGN(0x1000);
    .data   : { *(.data .data.*) } :data
    .tdata  : { *(.tdata .tdata.*) } :data :tls
    .tbss   : { *(.tbss .tbss.*) } :data :tls
    .bss    : { *(.bss .bss.*) } :data
    /DISCARD/ : { *(.comment) *(.eh_frame*) }
}
