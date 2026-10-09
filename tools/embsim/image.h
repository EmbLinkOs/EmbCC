/* image.h -- what the analyses know of the image beyond its bytes: the
 * symbol table, the code's address ranges, and the DWARF the compiler
 * left in it with -g: the line table (.debug_line), which maps an
 * address to a source line, and the call frame information
 * (.debug_frame), which says where each function keeps its caller's
 * registers. image.c reads them; the loader (loader.c) is not involved,
 * so a run without an analysis never opens the file twice. */
#ifndef EMBSIM_IMAGE_H
#define EMBSIM_IMAGE_H

#include "sim.h"

/* a symbol: a function (or a code label, sized up to the next), or any
 * named address */
struct img_sym {
    u32 addr, size;
    const char *name;
};

/* a line table row's range: [lo, hi) is line `line` of file `file` */
struct img_line {
    u32 lo, hi;
    int file, line;
};

#define IMG_CODE 8

struct image {
    u8 *f;
    size_t len;
    int e64;
    struct img_sym *fn;             /* the functions, by address */
    int nfn;
    struct img_sym *sym;            /* every named symbol, by name */
    int nsym;
    u32 code_lo[IMG_CODE], code_hi[IMG_CODE];   /* executable segments */
    int ncode;
    struct img_line *ln;            /* by address */
    int nln;
    char **file;                    /* the line table's files, as paths */
    int nfile;
    const u8 *frame;                /* .debug_frame, or 0 */
    u32 frame_len;
};

/* the image's tables; dies when the file cannot be read */
struct image *image_open(const char *path);
/* the function holding pc, by its index in m->fn, or -1 */
int image_fn(const struct image *m, u32 pc);
/* a symbol's address by its name: 1, or 0 when there is none */
int image_sym(const struct image *m, const char *name, u32 *addr);
/* the line table's row for pc, or 0 */
const struct img_line *image_line(const struct image *m, u32 pc);
/* "func+0x6 (file.c:12)", or as much of it as the image knows */
const char *image_where(const struct image *m, u32 pc, char *buf, size_t n);

/* ---- .debug_frame ------------------------------------------------------ */

#define CFI_REGS 64

enum { CFI_SAME, CFI_UNDEF, CFI_OFFSET, CFI_VALOFF, CFI_REG };

/* the rules at an address: the canonical frame address (a register plus
 * an offset) and where each register of the caller is */
struct cfi_row {
    int cfa_reg;
    s64 cfa_off;
    int ra;                         /* the return address's column */
    u8 how[CFI_REGS];
    s64 off[CFI_REGS];              /* CFI_OFFSET, CFI_VALOFF; CFI_REG's register */
};

/* the rules at pc: 1, or 0 when no FDE covers it or its CFI is beyond
 * what is understood (an expression) */
int cfi_at(const struct image *m, u32 pc, struct cfi_row *row);

#endif
