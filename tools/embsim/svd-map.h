/* svd-map.h -- a CMSIS-SVD file's peripherals as registers on the bus,
 * and the interface of the behavioural models layered over them.
 *
 * Every register of every peripheral the SVD describes (outside the
 * core's own private peripheral bus) is a word of storage with its reset
 * value, and an access to it does what the file says its fields do:
 * read-only, write-only, writeOnce; modifiedWriteValues (oneToClear and
 * the rest); readAction. An access where no register is, is a bus fault.
 * That is the register file (svd-map.c), and a peripheral without a model
 * is just that: its registers keep what is written, as the SVD allows.
 *
 * A model (stm32-rcc.c, stm32-gpio.c, ...) is a struct dev_ops over one
 * peripheral, picked by the board by the SVD's name for it. Its read and
 * write hooks get the program's accesses (`off` from the peripheral's
 * base) and pass them to rf_read and rf_write, the register file's, then
 * do what the hardware does: send the byte written to a data register,
 * set a ready bit, count. A change the hardware makes is rf_hw, which
 * --trace-periph shows as the hardware's. Its tick and next_event hooks
 * keep time as a device's do, while it says its clock runs (rf_clock).
 * docs/internals/embsim.md says how to add one. */
#ifndef EMBSIM_SVD_MAP_H
#define EMBSIM_SVD_MAP_H

#include "sim.h"

struct svd_periph;
struct svd_field;
struct svdmap;

/* a field's access, its modifiedWriteValues and its readAction */
enum { ACC_RW, ACC_RO, ACC_WO, ACC_W1, ACC_RW1 };   /* W1: writeOnce */
enum {
    MWV_MODIFY, MWV_1CLR, MWV_1SET, MWV_1TOG, MWV_0CLR, MWV_0SET, MWV_0TOG,
    MWV_CLEAR, MWV_SET
};
enum { RA_NONE, RA_CLEAR, RA_SET, RA_MODIFY };

struct rf_field {
    const char *name;               /* 0: a register without fields */
    int lsb, width;
    int acc, mwv, ract;
    const struct svd_field *svd;    /* its enumerated values, for the trace */
};

struct rf_reg {
    u32 addr;                       /* absolute */
    int bytes;                      /* 1, 2, 4 or 8 */
    u64 value, reset;
    u64 once;                       /* the writeOnce bits written since reset */
    const char *name;               /* in its peripheral: CR1, CH[2].CTRL */
    struct rf_periph *p;
    struct rf_field *f;
    int nf;
    int traced;                     /* --trace-periph shows it */
};

struct rf_periph {
    const char *name;
    u32 base, end;                  /* end: past its last register or block */
    struct rf_reg **reg;            /* its registers, by address */
    int nreg;
    const struct svd_periph *svd;
    struct sim *sim;
    /* its model's hooks and context, or the plain register file's */
    const struct dev_ops *ops;
    void *ctx;
    /* its clock: while `gate`'s `gate_mask` bits are clear, a read gives 0
     * and a write is ignored, as the part's peripherals do with their
     * clock off (a model sets it: stm32-rcc's) */
    struct rf_reg *gate;
    u64 gate_mask;
    const char *gate_name;          /* RCC.APB1ENR.USART2EN */
    int gate_warned;
};

/* a model type: the catalogue is in boards.c */
struct model_type {
    const char *name;
    const struct dev_ops *ops;
    /* its context, or 0 when the peripheral is not one it can model */
    void *(*create)(struct sim *s, struct rf_periph *p);
};
const struct model_type *model_find(const char *name);

/* the SVD's peripherals on the bus (sim_init calls it) */
struct svdmap *svdmap_create(struct sim *s, const char *path);
void svdmap_add(struct sim *s, struct svdmap *m);
/* the board's models over them */
void svdmap_models(struct sim *s, struct svdmap *m,
                   const struct model_desc *models);
/* --trace-periph: every peripheral (`which` 0), or those its comma list
 * names, as PERIPH or PERIPH.REGISTER with * for any characters */
void svdmap_trace(struct svdmap *m, const char *which);
/* --svd-map: every register of the file, and what EmbSim made of it */
void svdmap_print(struct svdmap *m, FILE *f);
/* the board's SVD file: in EMBSIM_SVD_PATH's directories, or 0 */
const char *svd_search(const char *name);

/* ---- for the models --------------------------------------------------- */

/* the peripheral of that name, or 0 */
struct rf_periph *rf_periph(struct svdmap *m, const char *name);
/* the i'th peripheral mapped, or 0 past the last */
struct rf_periph *rf_periph_at(struct svdmap *m, int i);
/* the register of that name in the peripheral, or 0 */
struct rf_reg *rf_reg(struct rf_periph *p, const char *name);
/* the bits of the register's field of that name, or 0 when it has none */
u64 rf_mask(const struct rf_reg *r, const char *field);
/* the program's read and write, as the SVD says the fields behave */
u32 rf_read(struct rf_periph *p, u32 off, int n);
void rf_write(struct rf_periph *p, u32 off, int n, u32 v);
/* the offset of a register from its peripheral's base */
static inline u32 rf_off(const struct rf_reg *r)
{
    return r->addr - r->p->base;
}
/* the hardware sets a register to `v` (whatever the SVD lets the program
 * do), and --trace-periph shows what changed */
void rf_hw(struct rf_reg *r, u64 v);
/* a model's correction of what the file says: the write behaviour of the
 * fields under `mask`, a reset value */
void rf_set_mwv(struct rf_reg *r, u64 mask, int mwv);
void rf_set_reset(struct rf_reg *r, u64 v);
/* the peripheral's interrupt (the SVD's): the first whose name has
 * `want` in it, else its first; -1 when it has none */
int rf_irq(const struct rf_periph *p, const char *want);
/* pend that interrupt in the core */
void rf_interrupt(struct rf_periph *p, int irq);
/* the model's clock starts (on 1) or stops: while it runs, its tick hook
 * is called after each instruction and its next_event asked (sim_clock,
 * for a model); `*running` is the model's own record of it */
void rf_clock(struct rf_periph *p, int *running, int on);

#endif
