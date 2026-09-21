/* COFF relocatable-object writer (D-014, Windows).
 *
 * The same shape as src/elf/write.h and src/macho/write.h -- create a
 * writer, add sections, add symbols, add relocations, write the file --
 * because the driver should differ between formats only where the
 * formats differ, and these three agree about what an object is.
 *
 * Where they do NOT agree, the difference is in the arguments here
 * rather than hidden inside:
 *
 *   No addend. A COFF relocation has nowhere to put one, so the caller
 *   patches the bytes being relocated before adding the section. This
 *   is Mach-O's rule too, and the opposite of ELF's RELA.
 *
 *   Alignment is a section CHARACTERISTIC, not a field, so
 *   coffw_add_section takes a byte count and encodes it.
 *
 *   Sections are numbered from one, and a symbol in section zero is
 *   undefined. coffw_add_section returns that one-based number, so it
 *   can be handed straight to coffw_add_symbol.
 */
#ifndef EMBCC_COFF_WRITE_H
#define EMBCC_COFF_WRITE_H

#include "coff.h"

struct coffw;

/* `machine` is IMAGE_FILE_MACHINE_*. Passed in rather than read from a
 * global, so the writer stays a library. */
struct coffw *coffw_new(int machine);
void coffw_free(struct coffw *w);

/* Returns the ONE-BASED section number, for use as a symbol's section.
 * `data` may be NULL when the section occupies no file space (.bss),
 * in which case `size` is still its memory size. `align` is a byte
 * count and must be a power of two from 1 to 64. */
int coffw_add_section(struct coffw *w, const char *name, unsigned flags,
                      const void *data, unsigned size, int align);

/* Returns the symbol table index, which is what a relocation names.
 * `section` is a number from coffw_add_section, or IMAGE_SYM_UNDEFINED
 * for a reference this object does not define. `value` is the offset
 * within that section for a definition, and for an undefined symbol it
 * is zero (nonzero would make it a COMMON). */
int coffw_add_symbol(struct coffw *w, const char *name, unsigned value,
                     int section, int type, int storage_class);

/* A relocation inside `section` at `off`, naming the symbol at index
 * `sym`. There is no addend: whatever the field already contains is
 * the addend, so it must be written into the section's bytes first. */
void coffw_add_reloc(struct coffw *w, int section, unsigned off,
                     int sym, int type);

/* Writes the object. Returns 0, or -1 with a message on stderr. */
int coffw_write(struct coffw *w, const char *path);

#endif
