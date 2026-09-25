/* EmbLD — the integrated linker (ARCHITECTURE §6, WORKPLAN stream B).
 *
 * Reads x86-64 ELF relocatable objects (and, later, static archives),
 * resolves symbols, lays out the EmbLink two-segment image, applies
 * relocations, and writes an ET_EXEC the in-kernel loader binds
 * (TARGET_ABI §4a: ET_EXEC never PIE, no PLT in a static link, the GOT
 * built AND filled).
 *
 * A library, not a subprocess: the target has no fork/exec
 * (ARCHITECTURE §1), so self-hosting needs one binary that compiles AND
 * links. This is exposed both as the standalone `embld` tool (for
 * host-side development and testing) and, wired into the driver, as
 * `embcc prog.c -o prog` linking in-process.
 */
#ifndef EMBCC_LINK_LINK_H
#define EMBCC_LINK_LINK_H

struct link_opts {
    const char *entry;      /* entry symbol; NULL = "_start" */
    unsigned long base;     /* text load address; see have_base */
    /* Whether `base` was given. A separate flag because ZERO is a real
     * address on a microcontroller -- flash starts there, and the reset
     * vector the processor fetches is the word at 0 -- so "0 means the
     * default" would make the one base a firmware image needs the one
     * it cannot ask for. */
    int have_base;
    int emit_embx;          /* 1 = write a native EMBX binary instead of ELF */
    unsigned long long caps;/* EMBX capability bitmask (bit == cap_id); 0 = none */
    /* L2: physical load address (p_paddr) = vaddr - lma_offset, for a
     * higher-half kernel whose LMA is its VMA minus KERNEL_VIRTUAL_BASE.
     * 0 = p_paddr == p_vaddr (the ordinary case). */
    unsigned long long lma_offset;
    /* A FIRMWARE layout: the writable segment is ADDRESSED here (SRAM)
     * but STORED immediately after the text in the image (flash), which
     * is what a microcontroller needs and what `lma_offset` above
     * cannot say -- that one shifts every segment by the same amount,
     * and here the two differ. 0 keeps the contiguous layout every
     * hosted target has always had.
     *
     * The linker then provides __data_load, __data_start, __data_end,
     * __bss_start and __bss_end, which is everything a startup routine
     * needs to copy .data out of flash and zero .bss -- so the startup
     * can be ordinary C with no linker script to keep in step. */
    unsigned long data_base;
};

/* Links inputs[0..n) into an ET_EXEC at `out`. Inputs are object files
 * (.o) or static archives (.a), resolved left to right as a linker
 * does. Returns 0, or 1 with a diagnostic on stderr. */
int embld_link(const char **inputs, int ninputs, const char *out,
               const struct link_opts *opts);

#endif
