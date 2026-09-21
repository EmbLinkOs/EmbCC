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
 * undefined one when sect is 0. `name` is the C name; the platform's
 * leading underscore is added here. Returns the symbol index. */
int machow_add_symbol(struct machow *w, const char *name,
                      unsigned long long value, int sect, int ext);

/* One relocation against `sect`. `offset` is from the start of that
 * section. `length` is the log2 of the patched field's width, `type` a
 * target-specific ARM64_RELOC_ or X86_64_RELOC_ value. An `addend` other
 * than zero is emitted the way the target requires — see write.c. */
void machow_add_reloc(struct machow *w, int sect, unsigned long long offset,
                      int sym, int type, int pcrel, int length, long addend);

/* Writes the MH_OBJECT file. Returns 0, or -1 with a message on stderr. */
int machow_write(struct machow *w, const char *path);

#endif
