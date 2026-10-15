/* atdf.c -- a Microchip ATDF file (the device packs' "AVR tools device
 * file", which describes their PIC32 and SAM parts as well as the AVRs)
 * read into the SVD reader's struct svd_device, so the register file
 * (svd-map.c) and its models take a PIC32 as they take an STM32.
 *
 * What it reads:
 *   - <devices><device><peripherals>: each module's instances; an
 *     instance with a register group at an address is a peripheral,
 *     named by the instance, its registers the module's group of that
 *     name. Where one group names the registers of every instance (a
 *     PIC32's TMR2 holds T2CON to T9CON, and TMR3 to TMR9 have no group),
 *     each instance gets those its number names. An instance in no
 *     address space (a PIC32's CORE: the GPRs as a debugger names them)
 *     is not on the bus.
 *   - <modules><module><register-group>: each <register> (name, offset,
 *     size in bytes, initval, rw) with its <bitfield>s (a mask, which may
 *     leave holes: each run of ones is a field, named NAME, then NAME_1
 *     ...), and their value groups' named values; a group nested by
 *     reference (name-in-module, offset, count) is flattened in.
 *   - <devices><device><interrupts>: the device's interrupts by index,
 *     which a PIC32's EVIC model finds by name (TIMER_2, UART1_TX...).
 * A PIC32 (architecture MIPS) has each 32-bit register followed by its
 * CLR, SET and INV registers, which the file leaves out: the register
 * file makes them (svd_device.clr_set_inv). */
#include <stdlib.h>
#include <string.h>

#include "../embsvd/svd.h"
#include "svd-map.h"

static unsigned long long num(const struct xnode *n, const char *a,
                              unsigned long long dflt)
{
    const char *v = svd_xattr(n, a);
    if (!v || !*v)
        return dflt;
    char *e;
    unsigned long long x = strtoull(v, &e, 0);
    if (*e)
        svd_die("line %d: %s=\"%s\" is not a number", svd_xline(n), a, v);
    return x;
}

static const char *acc_of(const char *rw, const char *dflt)
{
    if (!rw || !*rw)
        return dflt;
    if (!strcmp(rw, "R"))
        return "read-only";
    if (!strcmp(rw, "W"))
        return "write-only";
    return "read-write";
}

static const struct xnode *module_group(const struct xnode *modules,
                                        const char *module, const char *group)
{
    for (int i = 0; i < svd_xnkid(modules); i++) {
        const struct xnode *m = svd_xkid(modules, i);
        const char *mn = svd_xattr(m, "name");
        if (strcmp(svd_xname(m), "module") || !mn || (module && strcmp(mn, module)))
            continue;
        for (int k = 0; k < svd_xnkid(m); k++) {
            const struct xnode *g = svd_xkid(m, k);
            const char *gn = svd_xattr(g, "name");
            if (!strcmp(svd_xname(g), "register-group") && gn && !strcmp(gn, group))
                return g;
        }
    }
    return 0;
}

/* the named values of a value group, for --trace-periph */
static void read_values(struct svd_field *f, const struct xnode *module,
                        const char *vg)
{
    if (!vg || !module)
        return;
    for (int i = 0; i < svd_xnkid(module); i++) {
        const struct xnode *g = svd_xkid(module, i);
        const char *gn = svd_xattr(g, "name");
        if (strcmp(svd_xname(g), "value-group") || !gn || strcmp(gn, vg))
            continue;
        for (int k = 0; k < svd_xnkid(g); k++) {
            const struct xnode *v = svd_xkid(g, k);
            const char *vn = svd_xattr(v, "name");
            if (strcmp(svd_xname(v), "value") || !vn || !*vn)
                continue;
            f->ev = svd_grow(f->ev, (size_t)(f->nev + 1) * sizeof *f->ev);
            struct svd_enumval *e = &f->ev[f->nev++];
            memset(e, 0, sizeof *e);
            e->name = vn;
            e->desc = svd_xattr(v, "caption");
            e->value = num(v, "value", 0);
            e->care = ~0ull;
        }
        return;
    }
}

struct regs {
    struct svd_node *v;
    int n, cap;
};

static struct svd_node *new_reg(struct regs *r)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? 2 * r->cap : 64;
        r->v = svd_grow(r->v, (size_t)r->cap * sizeof *r->v);
    }
    struct svd_node *n = &r->v[r->n++];
    memset(n, 0, sizeof *n);
    return n;
}

static void read_group(struct regs *out, const struct xnode *modules,
                       const struct xnode *module, const struct xnode *g,
                       unsigned long long base, const char *prefix, int depth)
{
    if (depth > 8)
        svd_die("line %d: register groups nested too deep", svd_xline(g));
    for (int i = 0; i < svd_xnkid(g); i++) {
        const struct xnode *x = svd_xkid(g, i);
        if (!strcmp(svd_xname(x), "register-group")) {
            /* a group placed by reference, count times */
            const char *in = svd_xattr(x, "name-in-module");
            const struct xnode *sub = in ? module_group(modules, 0, in) : 0;
            if (!sub)
                continue;
            unsigned long long off = num(x, "offset", 0), cnt = num(x, "count", 1);
            unsigned long long size = num(x, "size", 0);
            const char *nm = svd_xattr(x, "name");
            for (unsigned long long k = 0; k < cnt; k++) {
                char pre[128];
                if (cnt > 1)
                    snprintf(pre, sizeof pre, "%s%s%llu_", prefix, nm ? nm : "",
                             k);
                else
                    snprintf(pre, sizeof pre, "%s", prefix);
                read_group(out, modules, module, sub, base + off + k * size,
                           pre, depth + 1);
            }
            continue;
        }
        if (strcmp(svd_xname(x), "register"))
            continue;
        const char *rn = svd_xattr(x, "name");
        if (!rn)
            svd_die("line %d: a register with no name", svd_xline(x));
        unsigned long long cnt = num(x, "count", 1);
        int bytes = (int)num(x, "size", 4);
        if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8)
            svd_die("line %d: register %s is %d bytes", svd_xline(x), rn, bytes);
        const char *racc = acc_of(svd_xattr(x, "rw"), "read-write");
        for (unsigned long long k = 0; k < cnt; k++) {
            struct svd_node *r = new_reg(out);
            size_t nl = strlen(prefix) + strlen(rn) + 24;
            char *name = svd_alloc(nl);
            if (cnt > 1)
                snprintf(name, nl, "%s%s%llu", prefix, rn, k);
            else
                snprintf(name, nl, "%s%s", prefix, rn);
            r->name = name;
            r->desc = svd_xattr(x, "caption");
            r->off = base + num(x, "offset", 0) + k * (unsigned long long)bytes;
            r->line = svd_xline(x);
            r->size = 8 * bytes;
            r->acc = racc;
            r->access = !strcmp(racc, "read-only") ? 1
                      : !strcmp(racc, "write-only") ? 2 : 0;
            r->reset = num(x, "initval", 0);
            r->rmask = bytes >= 8 ? ~0ull : (1ull << (8 * bytes)) - 1;
            /* the fields: each run of ones in a bitfield's mask */
            for (int b = 0; b < svd_xnkid(x); b++) {
                const struct xnode *bf = svd_xkid(x, b);
                const char *bn = svd_xattr(bf, "name");
                if (strcmp(svd_xname(bf), "bitfield") || !bn)
                    continue;
                unsigned long long m = num(bf, "mask", 0);
                const char *facc = acc_of(svd_xattr(bf, "rw"), racc);
                int part = 0;
                for (int lsb = 0; lsb < 64 && m; ) {
                    if (!(m >> lsb & 1)) {
                        lsb++;
                        continue;
                    }
                    int w = 0;
                    while (lsb + w < 64 && (m >> (lsb + w) & 1))
                        w++;
                    m &= ~((w >= 64 ? ~0ull : (1ull << w) - 1) << lsb);
                    r->f = svd_grow(r->f, (size_t)(r->nf + 1) * sizeof *r->f);
                    struct svd_field *f = &r->f[r->nf++];
                    memset(f, 0, sizeof *f);
                    if (part) {
                        size_t fl = strlen(bn) + 8;
                        char *fn = svd_alloc(fl);
                        snprintf(fn, fl, "%s_%d", bn, part);
                        f->name = fn;
                    } else {
                        f->name = bn;
                        read_values(f, module, svd_xattr(bf, "values"));
                    }
                    f->desc = svd_xattr(bf, "caption");
                    f->acc = facc;
                    f->lsb = lsb;
                    f->width = w;
                    part++;
                    lsb += w;
                }
            }
        }
    }
}

/* the number in a register's name that says whose it is: T5CON's 5,
 * U3MODE's 3, I2C4CON's 4 (past the module's own name), or -1 */
static long reg_unit(const char *reg, const char *module)
{
    size_t ml = strlen(module);
    const char *p = !strncmp(reg, module, ml) ? reg + ml : reg;
    while (*p && (*p < '0' || *p > '9'))
        p++;
    return *p ? strtol(p, 0, 10) : -1;
}

/* the number an instance's name ends with (TMR5's 5), or -1 */
static long inst_unit(const char *name)
{
    size_t n = strlen(name);
    while (n && name[n - 1] >= '0' && name[n - 1] <= '9')
        n--;
    return name[n] ? strtol(name + n, 0, 10) : -1;
}

static void add_periph(struct svd_device *d, const char *name, const char *mn,
                       unsigned long long base, struct svd_node *regs, int n)
{
    d->p = svd_grow(d->p, (size_t)(d->np + 1) * sizeof *d->p);
    struct svd_periph *p = &d->p[d->np++];
    memset(p, 0, sizeof *p);
    p->name = name;
    p->group = mn;
    p->base = base;
    p->kid = regs;
    p->nkid = n;
}

/* An instance's registers, as a peripheral. Where a module's one
 * register group names every instance's registers and the others have
 * none (a PIC32's TMR2 holds T2CON to T9CON, UART1 U1MODE to U6BRG),
 * each instance gets those its number names, from the lowest of them. */
static void split(struct svd_device *d, const struct xnode *mod,
                  const struct xnode *in, const char *mn,
                  unsigned long long base, struct regs *rs)
{
    const char *self = svd_xattr(in, "name");
    int others = 0;
    for (int k = 0; k < svd_xnkid(mod); k++) {
        const struct xnode *o = svd_xkid(mod, k);
        if (!strcmp(svd_xname(o), "instance") && o != in &&
            !svd_xfind(o, "register-group") && svd_xattr(o, "name"))
            others++;
    }
    if (!others) {
        add_periph(d, self, mn, base, rs->v, rs->n);
        return;
    }
    char *taken = svd_alloc((size_t)rs->n + 1);
    memset(taken, 0, (size_t)rs->n + 1);
    for (int k = 0; k <= svd_xnkid(mod); k++) {
        /* each other instance, then this one with what is left */
        const struct xnode *o = k < svd_xnkid(mod) ? svd_xkid(mod, k) : in;
        const char *on = svd_xattr(o, "name");
        if (strcmp(svd_xname(o), "instance") || !on ||
            (k < svd_xnkid(mod) && o == in))
            continue;
        long unit = inst_unit(on);
        struct svd_node *v = svd_alloc(sizeof *v * ((size_t)rs->n + 1));
        int n = 0;
        unsigned long long lo = ~0ull;
        for (int r = 0; r < rs->n; r++)
            if (!taken[r] && (o == in || (unit >= 0 &&
                                          reg_unit(rs->v[r].name, mn) == unit))) {
                taken[r] = 1;
                v[n++] = rs->v[r];
                if (rs->v[r].off < lo)
                    lo = rs->v[r].off;
            }
        if (!n) {
            free(v);
            continue;
        }
        for (int r = 0; r < n; r++)
            v[r].off -= lo;
        add_periph(d, on, mn, base + lo, v, n);
    }
    free(taken);
}

static int ull_cmp(const void *a, const void *b)
{
    unsigned long long x = *(const unsigned long long *)a;
    unsigned long long y = *(const unsigned long long *)b;
    return x < y ? -1 : x > y;
}

/* A PIC32's registers with CLR, SET and INV: those the device pack's
 * processor module (xc32/<part>/p<part>.S, beside atdf/) defines NAMECLR
 * for, four bytes after NAME. Without the module, svd-map.c's rule. */
static void read_sci(struct svd_device *d, const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t dl = slash ? (size_t)(slash - path) : 0;
    const char *part = d->name && !strncmp(d->name, "PIC", 3) ? d->name + 3
                                                               : d->name;
    if (!part)
        return;
    size_t l = dl + 2 * strlen(part) + 32;
    char *p = svd_alloc(l);
    snprintf(p, l, "%.*s%s../xc32/%s/p%s.S", (int)dl, path, dl ? "/" : "",
             part, part);
    FILE *f = fopen(p, "rb");
    if (!f) {
        free(p);
        return;
    }
    fclose(f);
    const char *file = svd_file;
    char *text = svd_read_all(p);
    svd_file = file;
    free(p);
    /* NAME = 0xADDR lines, by name */
    struct sym { const char *n; size_t len; unsigned long long a; } *v = 0;
    int nv = 0, cap = 0;
    for (char *q = text; *q; ) {
        char *e = strchr(q, '\n');
        if (e)
            *e = 0;
        char *eq = strstr(q, " = 0x");
        if (eq && q[0] != ' ' && q[0] != '#') {
            if (nv == cap) {
                cap = cap ? 2 * cap : 4096;
                v = svd_grow(v, (size_t)cap * sizeof *v);
            }
            v[nv].n = q;
            v[nv].len = (size_t)(eq - q);
            v[nv].a = strtoull(eq + 3, 0, 16) & 0x1fffffffull;
            nv++;
        }
        q = e ? e + 1 : q + strlen(q);
    }
    for (int i = 0; i < nv; i++) {
        if (v[i].len < 4 || strncmp(v[i].n + v[i].len - 3, "CLR", 3))
            continue;
        for (int k = i - 1; k >= 0 && k >= i - 2; k--)
            if (v[k].len == v[i].len - 3 && !strncmp(v[k].n, v[i].n, v[k].len) &&
                v[k].a + 4 == v[i].a) {
                d->sci_at = svd_grow(d->sci_at, (size_t)(d->nsci + 1) *
                                                    sizeof *d->sci_at);
                d->sci_at[d->nsci++] = v[k].a;
            }
    }
    free(v);
    free(text);
    qsort(d->sci_at, (size_t)d->nsci, sizeof *d->sci_at, ull_cmp);
}

struct svd_device *atdf_load(const char *path)
{
    const struct xnode *root = svd_xml_read(path);
    if (strcmp(svd_xname(root), "avr-tools-device-file"))
        svd_die("not an ATDF file: its root is <%s>", svd_xname(root));
    const struct xnode *devs = svd_xfind(root, "devices");
    const struct xnode *dev = svd_xfind(devs, "device");
    const struct xnode *modules = svd_xfind(root, "modules");
    if (!dev || !modules)
        svd_die("an ATDF file with no <device> or no <modules>");
    struct svd_device *d = svd_alloc(sizeof *d);
    memset(d, 0, sizeof *d);
    d->name = svd_xattr(dev, "name");
    d->vendor = "Microchip";
    d->cpu = svd_xattr(dev, "architecture");
    d->endian = "little";
    d->width = 32;
    d->aub = 8;
    d->clr_set_inv = d->cpu && !strcmp(d->cpu, "MIPS");
    const struct xnode *per = svd_xfind(dev, "peripherals");
    for (int i = 0; i < svd_xnkid(per); i++) {
        const struct xnode *mod = svd_xkid(per, i);
        const char *mn = svd_xattr(mod, "name");
        for (int k = 0; mn && k < svd_xnkid(mod); k++) {
            const struct xnode *in = svd_xkid(mod, k);
            const struct xnode *rg = svd_xfind(in, "register-group");
            const char *space = rg ? svd_xattr(rg, "address-space") : 0;
            if (strcmp(svd_xname(in), "instance") || !rg || !space || !*space)
                continue;
            const char *gm = svd_xattr(rg, "name-in-module");
            /* by its name in the module; a PIC32's TMR2 to TMR9 share
             * module TMR's one group, which names them all */
            const struct xnode *g = module_group(modules, mn, gm ? gm : mn);
            if (!g)
                g = module_group(modules, 0, gm ? gm : mn);
            if (!g)
                g = module_group(modules, mn, mn);
            if (!g)
                svd_die("line %d: instance %s's register group %s is in no "
                        "module", svd_xline(in), svd_xattr(in, "name"), gm);
            const struct xnode *module = 0;
            for (int q = 0; q < svd_xnkid(modules) && !module; q++) {
                const struct xnode *m = svd_xkid(modules, q);
                for (int z = 0; z < svd_xnkid(m); z++)
                    if (svd_xkid(m, z) == g)
                        module = m;
            }
            struct regs rs = { 0, 0, 0 };
            read_group(&rs, modules, module, g, 0, "", 0);
            split(d, mod, in, mn, num(rg, "offset", 0), &rs);
        }
    }
    for (int i = 0; i < d->np; i++)
        d->p[i].layout = &d->p[i];
    const struct xnode *irqs = svd_xfind(dev, "interrupts");
    for (int i = 0; i < svd_xnkid(irqs); i++) {
        const struct xnode *q = svd_xkid(irqs, i);
        const char *qn = svd_xattr(q, "name");
        if (strcmp(svd_xname(q), "interrupt") || !qn)
            continue;
        d->irq = svd_grow(d->irq, (size_t)(d->nirq + 1) * sizeof *d->irq);
        struct svd_irq *r = &d->irq[d->nirq++];
        r->name = qn;
        r->desc = svd_xattr(q, "caption");
        r->value = (int)num(q, "index", 0);
        if (r->value >= d->num_irq)
            d->num_irq = r->value + 1;
    }
    if (!d->np)
        svd_die("the device %s has no peripheral on the bus", d->name);
    if (d->clr_set_inv)
        read_sci(d, path);
    return d;
}
