/* `embcc -S`: the assembly the backend just emitted, as text. See asmout.c
 * for why it is produced from the bytes rather than by a second emitter. */
#ifndef EMBCC_ASMOUT_H
#define EMBCC_ASMOUT_H

struct outbuf;
struct unit;
struct ir_unit;
struct extcall;
struct strsite;
struct gsite;
struct fsite;

void asm_emit_unit(struct outbuf *b, const char *srcname, struct unit *u,
                   struct ir_unit *iu, const unsigned char *text, long textlen,
                   const unsigned char *rodata,
                   struct extcall *ext, int next,
                   struct strsite *strs, int nstrs,
                   struct gsite *gs, int ngs,
                   struct fsite *fs, int nfs);

/* main.c: where each string literal goes (.rodata or a mergeable string
 * section), and the string an old .rodata offset falls in */
void str_place(struct ir_unit *iu, int merge);
int str_at(const struct ir_unit *iu, long off, long *delta);

#endif
