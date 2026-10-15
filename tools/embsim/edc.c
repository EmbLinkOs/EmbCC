/* edc.c -- a Microchip EDC file (the device packs' ".PIC" files, MPLAB's
 * description of a 16-bit part: PIC24 and dsPIC) read into the SVD
 * reader's struct svd_device, so the register file (svd-map.c) and its
 * models take a PIC24 as they take an STM32 or a PIC32.
 *
 * What it reads:
 *   - <edc:DataSpace>'s SFR sectors: each <edc:SFRDef>, a 16-bit register
 *     at its data-space address (edc:_addr, else counted from the
 *     sector's start with its <edc:AdjustPoint> gaps), its value at
 *     power-on (edc:por, a character a bit from the top: 1, 0, - for
 *     unimplemented, x for unknown), the access of each bit (edc:access:
 *     n read-write, r read-only, w write-only, c and s clearable and
 *     settable), and the fields of its datasheet mode (<edc:SFRMode
 *     edc:id="DS.0">: <edc:SFRFieldDef> mask and width, placed from bit 0
 *     up past <edc:AdjustPoint> gaps). A <edc:JoinedSFRDef> (PC) is the
 *     registers it joins.
 *   - <edc:InterruptList>: each <edc:Interrupt> by its number (edc:irq),
 *     named less "Interrupt" (T1, ADC1, U1RX), which the PIC24's
 *     interrupt controller model finds by name;
 *   - <edc:ProgramSpace>: where program memory ends (the first
 *     configuration sector's end, the single-partition part's), and each
 *     configuration word (<edc:DCRDef>) at its address there; and the
 *     RAM, <edc:DataSpace>'s span (svd_device.code_end, ram_base,
 *     ram_end, cfg);
 *   - <edc:PinList>: each <edc:Pin>'s names (its <edc:VirtualPin>s: RP21,
 *     OCM1A, PMA5, RG6...) on the port pin among them (RG6), which the
 *     ports' model puts the peripherals' outputs on (svd_device.pin).
 * The file has no peripherals, only the module each register comes from
 * (edc:_modsrc): a run of registers from one module, unbroken by another
 * module's, is a peripheral, named after its first register (table
 * below: TRISA's ports are GPIO, U1MODE's UARTs UART), else by that
 * register's letters (SPI1CON1L: SPI); a later run of the same module
 * and name joins it (CCP4-7, past the comparators). The core's registers (W0 to
 * TBLPAG, its module's) are the core's: no peripheral, as an ATDF's CORE
 * instance is none.
 *
 * Every register's bits that no field covers and that are implemented
 * get a field (named after the register, its bit numbers after it), so
 * the register file keeps what a program writes there, as the part does;
 * the unimplemented bits read 0 and ignore writes. The data space is on
 * EmbSim's bus at PIC24_DATA (svd_device.bus_offset), and the SFR space
 * between registers reads as 0 (svd_device.holes_zero), as the part's
 * does. */
#include <stdlib.h>
#include <string.h>

#include "../embsvd/svd.h"
#include "pic24.h"
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

static int is(const struct xnode *n, const char *tag)
{
    return !strcmp(svd_xname(n), tag);
}

/* the access of bits [lsb, lsb + w) from an access string (its first
 * character is the top bit) */
static const char *bits_acc(const char *acc, int nbits, int lsb, int w)
{
    int ro = 0, wo = 0, n = 0;
    for (int b = lsb; b < lsb + w; b++) {
        int i = nbits - 1 - b;
        char c = acc && i >= 0 && i < (int)strlen(acc) ? acc[i] : 'n';
        if (c == '-')
            continue;
        n++;
        ro += c == 'r';
        wo += c == 'w';
    }
    return n && ro == n ? "read-only" : n && wo == n ? "write-only" : "read-write";
}

static void add_field(struct svd_node *r, const char *name, int lsb, int w,
                      const char *acc)
{
    r->f = svd_grow(r->f, (size_t)(r->nf + 1) * sizeof *r->f);
    struct svd_field *f = &r->f[r->nf++];
    memset(f, 0, sizeof *f);
    f->name = name;
    f->lsb = lsb;
    f->width = w;
    f->acc = acc;
}

/* one SFR */
static void read_sfr(struct svd_node *r, const struct xnode *x,
                     unsigned long long addr)
{
    memset(r, 0, sizeof *r);
    r->name = svd_xattr(x, "edc:cname");
    if (!r->name)
        r->name = svd_xattr(x, "edc:name");
    if (!r->name)
        svd_die("line %d: an SFR with no name", svd_xline(x));
    r->desc = svd_xattr(x, "edc:desc");
    r->off = addr;
    r->line = svd_xline(x);
    int nbits = (int)num(x, "edc:nzwidth", 16);
    if (nbits != 8 && nbits != 16 && nbits != 32)
        svd_die("line %d: SFR %s is %d bits", svd_xline(x), r->name, nbits);
    r->size = nbits < 16 ? 16 : nbits;
    r->rmask = (1ull << r->size) - 1;
    const char *por = svd_xattr(x, "edc:por");
    const char *acc = svd_xattr(x, "edc:access");
    unsigned long long impl = 0;
    for (int b = 0; b < nbits; b++) {
        int i = nbits - 1 - b;
        if (por && i < (int)strlen(por) && por[i] == '1')
            r->reset |= 1ull << b;
        if (!acc || (i < (int)strlen(acc) && acc[i] != '-'))
            impl |= 1ull << b;
    }
    r->acc = bits_acc(acc, nbits, 0, nbits);
    r->access = !strcmp(r->acc, "read-only") ? 1
              : !strcmp(r->acc, "write-only") ? 2 : 0;
    /* the datasheet mode's fields */
    const struct xnode *ml = svd_xfind(x, "edc:SFRModeList");
    unsigned long long covered = 0;
    for (int m = 0; ml && m < svd_xnkid(ml); m++) {
        const struct xnode *mode = svd_xkid(ml, m);
        const char *id = svd_xattr(mode, "edc:id");
        if (!is(mode, "edc:SFRMode") || !id || strcmp(id, "DS.0"))
            continue;
        int at = 0;
        for (int k = 0; k < svd_xnkid(mode); k++) {
            const struct xnode *fd = svd_xkid(mode, k);
            if (is(fd, "edc:AdjustPoint")) {
                at += (int)num(fd, "edc:offset", 0);
                continue;
            }
            if (!is(fd, "edc:SFRFieldDef"))
                continue;
            int w = (int)num(fd, "edc:nzwidth", 1);
            const char *fn = svd_xattr(fd, "edc:cname");
            if (w < 1 || at + w > r->size || !fn)
                svd_die("line %d: field %s of %s is outside it", svd_xline(fd),
                        fn ? fn : "?", r->name);
            add_field(r, fn, at, w, bits_acc(acc, nbits, at, w));
            r->f[r->nf - 1].desc = svd_xattr(fd, "edc:desc");
            covered |= ((1ull << w) - 1) << at;
            at += w;
        }
        break;
    }
    /* implemented bits no field names: fields of their own, so a write
     * to them stays */
    unsigned long long rest = impl & ~covered;
    for (int lsb = 0; lsb < r->size && rest; ) {
        if (!(rest >> lsb & 1)) {
            lsb++;
            continue;
        }
        int w = 0;
        while (lsb + w < r->size && (rest >> (lsb + w) & 1))
            w++;
        rest &= ~(((1ull << w) - 1) << lsb);
        size_t l = strlen(r->name) + 16;
        char *fn = svd_alloc(l);
        if (w == r->size)
            snprintf(fn, l, "%s", r->name);
        else if (w == 1)
            snprintf(fn, l, "%s_%d", r->name, lsb);
        else
            snprintf(fn, l, "%s_%d_%d", r->name, lsb + w - 1, lsb);
        add_field(r, fn, lsb, w, bits_acc(acc, nbits, lsb, w));
        lsb += w;
    }
}

struct sfr {
    struct svd_node r;
    const char *mod;
};
struct sfrs {
    struct sfr *v;
    int n, cap;
};

static void collect(struct sfrs *out, const struct xnode *sector)
{
    unsigned long long at = num(sector, "edc:beginaddr", 0);
    for (int i = 0; i < svd_xnkid(sector); i++) {
        const struct xnode *x = svd_xkid(sector, i);
        if (is(x, "edc:AdjustPoint")) {
            at += num(x, "edc:offset", 0);
            continue;
        }
        if (is(x, "edc:JoinedSFRDef")) {
            for (int k = 0; k < svd_xnkid(x); k++) {
                const struct xnode *y = svd_xkid(x, k);
                if (!is(y, "edc:SFRDef"))
                    continue;
                if (out->n == out->cap) {
                    out->cap = out->cap ? 2 * out->cap : 512;
                    out->v = svd_grow(out->v, (size_t)out->cap * sizeof *out->v);
                }
                unsigned long long a = num(y, "edc:_addr", at);
                read_sfr(&out->v[out->n].r, y, a);
                out->v[out->n].mod = svd_xattr(y, "edc:_modsrc");
                out->n++;
                at = a + 2;
            }
            continue;
        }
        if (!is(x, "edc:SFRDef"))
            continue;
        if (out->n == out->cap) {
            out->cap = out->cap ? 2 * out->cap : 512;
            out->v = svd_grow(out->v, (size_t)out->cap * sizeof *out->v);
        }
        unsigned long long a = num(x, "edc:_addr", at);
        read_sfr(&out->v[out->n].r, x, a);
        out->v[out->n].mod = svd_xattr(x, "edc:_modsrc");
        out->n++;
        at = a + (unsigned long long)(num(x, "edc:nzwidth", 16) + 7) / 8;
        if (at & 1)
            at++;
    }
}

static void find_sectors(struct sfrs *out, const struct xnode *n, int depth)
{
    for (int i = 0; depth < 8 && i < svd_xnkid(n); i++) {
        const struct xnode *k = svd_xkid(n, i);
        if (is(k, "edc:SFRDataSector"))
            collect(out, k);
        else if (!is(k, "edc:SFRDef"))
            find_sectors(out, k, depth + 1);
    }
}

/* a peripheral's name by its first register: the family's own, where
 * the register's letters say little */
static const struct { const char *first, *name; } names[] = {
    { "INTCON1", "INT" }, { "OSCCON", "OSC" }, { "PMCON1", "PMP" },
    { "CRCCON1", "CRC" }, { "REFOCON", "REFO" }, { "CTMUCON1", "CTMU" },
    { "RTCCON1", "RTCC" }, { "CMSTAT", "CMP" }, { "U1MODE", "UART" },
    { "I2C1", "I2C" }, { "DMACON", "DMA" }, { "U1OTG", "USB" },
    { "TRISA", "GPIO" }, { "TRISB", "GPIO" }, { "PADCON", "PAD" },
    { "NVMCON", "NVM" }, { "RPINR", "PPS" }, { "RPOR", "PPS" },
    { "HLVDCON", "HLVD" }, { "ADC1BUF", "ADC1" }, { "AD1CON1", "ADC1" },
    { "TMR1", "TMR" }, { "IC1CON", "IC" }, { "OC1CON", "OC" },
    { "CCP1CON", "CCP" }, { "SPI1CON", "SPI" }, { "CLC1CON", "CLC" },
    { "PMD1", "PMD" }, { "RCON", "RCON" }, { "ANCFG", "ANCFG" },
};

static char *periph_name(const char *first)
{
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strncmp(first, names[i].first, strlen(names[i].first)))
            return svd_dup(names[i].name, strlen(names[i].name));
    size_t n = 0;
    while (first[n] >= 'A' && first[n] <= 'Z')
        n++;
    return svd_dup(first, n ? n : strlen(first));
}

static int is_core(const char *mod)
{
    return mod && strstr(mod, "CPU");
}

/* a port pin's name: R, its port's letter (A to K, no I), its bit */
static int port_pin(const char *n, int *port, int *bit)
{
    if (!n || n[0] != 'R' || n[1] < 'A' || n[1] > 'K' || n[1] == 'I' ||
        n[2] < '0' || n[2] > '9')
        return 0;
    char *e;
    long b = strtol(n + 2, &e, 10);
    if (*e || b > 31)
        return 0;
    *port = n[1] - 'A' - (n[1] > 'I');
    *bit = (int)b;
    return 1;
}

static void read_pins(struct svd_device *d, const struct xnode *pl)
{
    for (int i = 0; pl && i < svd_xnkid(pl); i++) {
        const struct xnode *p = svd_xkid(pl, i);
        int port = -1, bit = -1, n = 0;
        if (!is(p, "edc:Pin"))
            continue;
        for (int j = 0; j < svd_xnkid(p); j++)
            if (port_pin(svd_xattr(svd_xkid(p, j), "edc:name"), &port, &bit))
                n++;
        if (n != 1)
            continue;               /* a supply, or no port */
        for (int j = 0; j < svd_xnkid(p); j++) {
            const char *nm = svd_xattr(svd_xkid(p, j), "edc:name");
            if (!is(svd_xkid(p, j), "edc:VirtualPin") || !nm)
                continue;
            d->pin = svd_grow(d->pin, (size_t)(d->npin + 1) * sizeof *d->pin);
            d->pin[d->npin].name = nm;
            d->pin[d->npin].port = port;
            d->pin[d->npin++].bit = bit;
        }
    }
}

/* the program space: its end (the first code sector's, to its page's
 * end, where the configuration words are), the configuration words (each
 * the first time it is named: the single-partition part's) */
static void read_program(struct svd_device *d, const struct xnode *n, int depth)
{
    for (int i = 0; n && depth < 8 && i < svd_xnkid(n); i++) {
        const struct xnode *k = svd_xkid(n, i);
        const char *rid = svd_xattr(k, "edc:regionid");
        if (is(k, "edc:CodeSector") && !d->code_end && rid && !strcmp(rid, "program"))
            d->code_end = (num(k, "edc:endaddr", 0) + 0x7ff) & ~0x7ffull;
        if (is(k, "edc:DCRDef")) {
            const char *nm = svd_xattr(k, "edc:cname");
            int seen = 0;
            for (int j = 0; nm && j < d->ncfg; j++)
                seen |= !strcmp(d->cfg[j].name, nm);
            if (nm && !seen) {
                d->cfg = svd_grow(d->cfg, (size_t)(d->ncfg + 1) * sizeof *d->cfg);
                d->cfg[d->ncfg].name = nm;
                d->cfg[d->ncfg++].addr = num(k, "edc:_addr", 0);
            }
            continue;
        }
        read_program(d, k, depth + 1);
    }
}

struct svd_device *edc_load(const char *path)
{
    const struct xnode *root = svd_xml_read(path);
    if (!is(root, "edc:PIC"))
        svd_die("not an EDC file: its root is <%s>", svd_xname(root));
    struct svd_device *d = svd_alloc(sizeof *d);
    memset(d, 0, sizeof *d);
    d->name = svd_xattr(root, "edc:name");
    d->vendor = "Microchip";
    d->cpu = svd_xattr(root, "edc:arch");
    d->endian = "little";
    d->width = 16;
    d->aub = 8;
    d->bus_offset = PIC24_DATA;
    d->holes_zero = 1;
    const struct xnode *ds = svd_xfind(root, "edc:DataSpace");
    if (!ds)
        svd_die("an EDC file with no <edc:DataSpace>");
    d->ram_base = num(ds, "edc:xbeginaddr", 0);
    d->ram_end = num(ds, "edc:xendaddr", 0);
    read_program(d, svd_xfind(root, "edc:ProgramSpace"), 0);
    struct sfrs all = { 0, 0, 0 };
    find_sectors(&all, ds, 0);
    /* runs of one module's registers: the peripherals */
    for (int i = 0; i < all.n; ) {
        int j = i + 1;
        while (j < all.n && all.v[j].mod && all.v[i].mod &&
               !strcmp(all.v[j].mod, all.v[i].mod))
            j++;
        if (is_core(all.v[i].mod)) {
            i = j;
            continue;
        }
        const char *nm = periph_name(all.v[i].r.name);
        /* a module whose registers another module's interrupt (CCP1-3,
         * then CCP4-7 past the comparators): one peripheral still */
        struct svd_periph *p = 0;
        for (int k = 0; k < d->np; k++)
            if (!strcmp(d->p[k].name, nm) && d->p[k].group && all.v[i].mod &&
                !strcmp(d->p[k].group, all.v[i].mod))
                p = &d->p[k];
        if (!p) {
            /* a name a run of another module had (the device's own
             * module's blocks): numbered */
            int dup = 0;
            for (int k = 0; k < d->np; k++)
                if (!strcmp(d->p[k].name, nm))
                    dup++;
            if (dup) {
                size_t l = strlen(nm) + 16;
                char *n2 = svd_alloc(l);
                snprintf(n2, l, "%s_%d", nm, dup + 1);
                nm = n2;
            }
            d->p = svd_grow(d->p, (size_t)(d->np + 1) * sizeof *d->p);
            p = &d->p[d->np++];
            memset(p, 0, sizeof *p);
            p->name = nm;
            p->group = all.v[i].mod;
            p->base = all.v[i].r.off;
        }
        p->kid = svd_grow(p->kid, sizeof *p->kid * (size_t)(p->nkid + j - i));
        for (int k = i; k < j; k++) {
            p->kid[p->nkid] = all.v[k].r;
            p->kid[p->nkid++].off -= p->base;
        }
        i = j;
    }
    free(all.v);
    for (int i = 0; i < d->np; i++)
        d->p[i].layout = &d->p[i];
    /* the interrupts, by number */
    const struct xnode *il = svd_xfind(root, "edc:InterruptList");
    for (int i = 0; il && i < svd_xnkid(il); i++) {
        const struct xnode *x = svd_xkid(il, i);
        const char *cn = svd_xattr(x, "edc:cname");
        if (!is(x, "edc:Interrupt") || !cn)
            continue;
        size_t l = strlen(cn);
        if (l > 9 && !strcmp(cn + l - 9, "Interrupt"))
            l -= 9;
        d->irq = svd_grow(d->irq, (size_t)(d->nirq + 1) * sizeof *d->irq);
        struct svd_irq *q = &d->irq[d->nirq++];
        q->name = svd_dup(cn, l);
        q->desc = svd_xattr(x, "edc:desc");
        q->value = (int)num(x, "edc:irq", 0);
        if (q->value + 1 > d->num_irq)
            d->num_irq = q->value + 1;
    }
    read_pins(d, svd_xfind(root, "edc:PinList"));
    return d;
}
