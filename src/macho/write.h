/* Mach-O relocatable-object writer (D-014).
 *
 * Deliberately shaped like src/elf/write.h — create, append sections and
 * symbols, add relocations, write — because the driver has to drive both
 * and the two should differ only where the formats genuinely do.
 *
 * They are NOT unified behind one interface yet, and that is on purpose:
 * D-011 settled this argument once already for the two backends, and the
 * answer was to derive the shared shape from two WORKING implementations
 * rather than invent it from one. The same applies here. When Mach-O
 * emits everything ELF does, the common half will be visible rather than
 * guessed at.
 */
#ifndef EMBCC_MACHO_WRITE_H
#define EMBCC_MACHO_WRITE_H

#include "macho.h"

struct machow;

/* `cputype` and `cpusubtype` are the CPU_TYPE_ and CPU_SUBTYPE_
 * constants, passed
 * in rather than read from a global for the reason elfw_new gives:
 * this stays a library. */
struct machow *machow_new(int cputype, int cpusubtype);
void machow_free(struct machow *w);

/* Returns the section index, 1-based as a symbol's n_sect wants it.
 * `align` is the alignment as a power of two, given as the EXPONENT
 * (4 means 16 bytes) — the field is stored that way and converting at
 * the edge keeps one representation in play. data may be NULL when the
 * section is zero-filled. */
int machow_add_section(struct machow *w, const char *segname,
                       const char *sectname, unsigned int flags,
                       const void *data, unsigned long long size,
                       unsigned int align);

/* A defined symbol in section `sect` (from machow_add_section), or an
 * undefined one when sect is 0. `value` is the offset WITHIN that
 * section; the section's base is added here, because a Mach-O symbol
 * records an address and an ELF one in a relocatable object records an
 * offset, and every caller of both would otherwise have to remember
 * which it was talking to.
 *
 * Returns a 1-BASED handle, so that 0 can mean "no symbol yet" -- which
 * is what callers want, and what ELF gets for free from its reserved
 * index 0. Mach-O has no such reserved entry: index 0 there is an
 * ordinary symbol, and a caller using 0 as a sentinel silently
 * relocates against whichever symbol happened to be added first. */
int machow_add_symbol(struct machow *w, const char *name,
                      unsigned long long value, int sect, int ext);

/* The same, marked weak. An undefined weak reference is allowed to stay
 * unresolved and read as zero -- which is what the OS seam's optional
 * groups are built on (lib/libc/os/backend.h). */
int machow_add_symbol_weak(struct machow *w, const char *name,
                           unsigned long long value, int sect, int ext);

/* The same, with the name taken EXACTLY as given -- no underscore.
 * Assembler temporaries (`ltmp0`, the anchor a section-relative
 * reference relocates against) have no C name to prefix, and ld strips
 * a symbol whose name begins with 'l' or 'L' from the final table,
 * which is the whole reason they are spelled that way. */
int machow_add_symbol_raw(struct machow *w, const char *name,
                          unsigned long long value, int sect, int ext);

/* One relocation against `sect`. `offset` is from the start of that
 * section. `length` is the log2 of the patched field's width, `type` a
 * target-specific ARM64_RELOC_ or X86_64_RELOC_ value. An `addend` other
 * than zero is emitted the way the target requires — see write.c. */
void machow_add_reloc(struct machow *w, int sect, unsigned long long offset,
                      int sym, int type, int pcrel, int length, long addend);

/* "target - here", which Mach-O has no single relocation for: it is a
 * SUBTRACTOR naming the symbol to subtract, immediately followed by an
 * UNSIGNED naming the target, both at the same address. The linker
 * computes target - minuend + whatever the field already holds.
 *
 * `minuend` is an anchor symbol at the start of the section the field
 * is in, because SUBTRACTOR needs a SYMBOL and "here" is not one. The
 * caller stores (addend - offset_of_field) in the field, which turns
 * the anchor-relative answer into a field-relative one. */
void machow_add_reloc_sub(struct machow *w, int sect,
                          unsigned long long offset, int minuend, int target,
                          int length);

/* Where a section was placed in this object's address space. Needed
 * because a section-relative relocation's field holds the target's
 * ADDRESS, not its offset within the section -- the same rule a
 * symbol's n_value follows, and just as easy to miss when __text
 * happens to sit at zero and makes the two look alike. */
unsigned long long machow_section_addr(struct machow *w, int sect);

/* A relocation against a SECTION rather than a symbol (extern=0). The
 * field holds the target's address within this object, and the linker
 * adjusts it by however far that section moved. Mach-O has no section
 * symbols to relocate against the way ELF does, so this is how a
 * reference to "somewhere in __text" is spelled. `sect_target` is a
 * 1-based section index. */
void machow_add_reloc_sect(struct machow *w, int sect,
                           unsigned long long offset, int sect_target,
                           int length);

/* Writes the MH_OBJECT file. Returns 0, or -1 with a message on stderr. */
int machow_write(struct machow *w, const char *path);

#endif
