/* svd.h -- the CMSIS-SVD reader that embsvd and embsim share.
 *
 * svd_load reads an SVD file into a struct svd_device: its peripherals,
 * each peripheral's registers and clusters as the file describes them
 * (an array is one node, with dim and dimIncrement), derivedFrom resolved,
 * and every cluster struct sized and checked as svdconv lays it out.
 * svd_flat_regs lists one peripheral's registers flattened: each element
 * of every array, at its offset from the peripheral's base, with the name
 * and the C path it has in the header.
 *
 * What the reader cannot place where the file says is refused by name
 * and line (svd_die: "PROG: FILE: message", then exit(svd_status)).
 * ISO C99, standalone. */
#ifndef EMBSVD_SVD_H
#define EMBSVD_SVD_H

#include <stdio.h>

struct xnode;

struct svd_props {                  /* what a register inherits */
    int size;
    const char *acc;
    unsigned long long reset, rmask;
    int rmask_set;
};
struct svd_enumval {
    const char *name, *desc, *usage;
    unsigned long long value, care;   /* care: the bits that are not x */
    int isdef;
};
struct svd_field {
    const char *name, *desc, *acc;
    /* modifiedWriteValues and readAction as written, the register's when
     * the field has none, or 0 */
    const char *mwv, *ract;
    int lsb, width;
    struct svd_enumval *ev;
    int nev;
};
/* a register or a cluster, as the SVD describes it: an array is one node */
struct svd_node {
    int cluster;
    const char *name;           /* as written: may hold %s or [%s] */
    const char *desc, *alt;
    unsigned long long off;     /* from the enclosing cluster or peripheral */
    int dim;                    /* elements, or 0 when it is not an array */
    unsigned long long inc;
    char **idx;                 /* each element's dimIndex name */
    int bracket;                /* NAME[%s] */
    int line;
    /* a register */
    int size, access;           /* bits; 0 rw, 1 read-only, 2 write-only */
    const char *acc;
    const char *mwv, *ract;     /* modifiedWriteValues, readAction, or 0 */
    unsigned long long reset, rmask;
    struct svd_field *f;
    int nf;
    /* a cluster */
    struct svd_node *kid;
    int nkid;
    struct svd_node *type;          /* whose struct it is: its own, or shared */
    char *tname;                /* that struct's name, less _Type */
    const struct xnode *key;    /* where its registers were read from */
    struct svd_props pr;
    unsigned long long tsize, tend;
    unsigned talign;
    int sized, locked, emitted;
};
struct svd_irq { const char *name, *desc; int value; };
struct svd_ablock { unsigned long long off, size; const char *usage; };
struct svd_periph {
    const char *name, *desc, *group, *derived;
    char *tname;                /* its struct's name, less _Type */
    unsigned long long base;
    struct svd_node *kid;
    int nkid;
    struct svd_periph *layout;      /* whose registers these are */
    struct xnode *node;
    struct svd_ablock *ab;
    int nab;
    struct svd_irq *irq;            /* its own interrupts */
    int nirq;
};
struct svd_device {
    const char *name, *vendor, *version, *desc, *prefix;
    const char *cpu, *cpurev, *endian;
    int aub, width, has_cpu, prio_bits, fpu, fpu_dp, mpu, vendor_systick;
    int num_irq;
    struct svd_periph *p;
    int np;
    struct svd_irq *irq;
    int nirq;
};

struct svd_flat {
    const struct svd_node *r;
    char *name, *path;
    int idx[40];
    int nidx;
    unsigned long long off;
};
struct svd_flats { struct svd_flat *v; int n, cap; };

/* what svd_die prints first (default "embsvd"), the status it exits
 * with (default 1), and whether the reader's warnings are kept quiet */
extern const char *svd_prog;
extern int svd_status, svd_quiet;
/* the file being read, as messages name it */
extern const char *svd_file;
/* every cluster struct, to share and to name */
extern struct svd_node **svd_types;
extern int svd_ntypes;

/* read, resolve, lay out and check an SVD file; dies with its message */
struct svd_device *svd_load(const char *path);

void svd_die(const char *fmt, ...);
void *svd_alloc(size_t n);
void *svd_grow(void *p, size_t n);
char *svd_dup(const char *s, size_t n);
char *svd_read_all(const char *path);
/* SVD's scaledNonNegativeInteger */
unsigned long long svd_num(const char *s, const char *what, int line);
/* NAME with [%s] or %s taken out */
char *svd_plain_name(const char *s);
/* NAME%s with "with" for the %s; strip makes NAME[%s] NAMEwith */
char *svd_subst(const char *name, const char *with, int strip);
/* a C identifier from an SVD name (a static buffer, four in turn) */
const char *svd_ident(const char *s);
/* a description, without anything that would end a C comment */
void svd_put_desc(FILE *f, const char *s);
/* one struct's members laid out: checked, and with f printed */
unsigned long long svd_lay(const struct svd_node *kids, int nkid, FILE *f,
                           const char *what, unsigned *align, int *nres);
/* a peripheral's registers, flattened; sorted by offset if `sorted` */
struct svd_flats svd_flat_regs(const struct svd_periph *p, int sorted);
void svd_free_flats(struct svd_flats *o);
void svd_size_device(struct svd_device *d);

#endif
