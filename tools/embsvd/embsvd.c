/* embsvd -- a microcontroller's register database, from its CMSIS-SVD file.
 *
 * Every Cortex-M vendor publishes an SVD: an XML description of the
 * device's peripherals, their registers and bit fields, and its
 * interrupts. From it this tool writes what a project starts with:
 *
 *   --header FILE    the device header, in the shape CMSIS's svdconv and
 *                    the vendors' own headers have: IRQn_Type, one struct
 *                    per peripheral layout, base addresses, instance
 *                    pointers, and _Pos/_Msk for every field
 *   --startup FILE   a startup file in C: the vector table, with a weak
 *                    handler per interrupt, and a Reset_Handler that
 *                    copies .data, zeroes .bss, runs the constructors and
 *                    calls main
 *   --ld FILE        a linker script for it, in STM32CubeMX's shape, given
 *                    --flash ORIGIN:LENGTH and --ram ORIGIN:LENGTH (an SVD
 *                    describes peripherals, not memories)
 *   --nvic-prio-bits N, --fpu-present 0|1
 *                    what the header tells CMSIS about the core, where the
 *                    vendor's SVD is wrong (ST's STM32F405.svd 1.2 says 3
 *                    priority bits and no FPU; the part has 4 and one)
 *   --list           the peripherals, their addresses and interrupts
 *   --show NAME      one peripheral's registers and fields
 *
 * What an SVD can say that this does not handle -- clusters, a register
 * array whose name is not NAME%s or NAME[%s] -- is refused by name rather
 * than written approximately: a header whose offsets are wrong compiles
 * and then drives the wrong register.
 *
 * ISO C and standalone, like embar: no part of the compiler is needed.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_file = "?";

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "embsvd: %s: ", g_file);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "embsvd: out of memory\n");
        exit(1);
    }
    return p;
}

static void *xgrow(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "embsvd: out of memory\n");
        exit(1);
    }
    return p;
}

static char *xdup(const char *s, size_t n)
{
    char *r = xalloc(n + 1);
    memcpy(r, s, n);
    return r;
}

/* ---- a small XML reader ---------------------------------------------
 *
 * Enough of XML for SVD: elements, attributes, text, comments,
 * processing instructions, a DOCTYPE, CDATA and the five predefined
 * entities plus numeric ones. No namespaces are interpreted (an
 * attribute's prefix stays in its name). */

struct xattr { char *name, *value; };
struct xnode {
    char *name;
    char *text;                 /* the text directly inside, trimmed */
    struct xattr *attr;
    int nattr;
    struct xnode **kid;
    int nkid, capkid;
    int line;
};

struct xp { const char *s, *p; int line; };

static void xp_adv(struct xp *x, size_t n)
{
    while (n-- && *x->p) {
        if (*x->p == '\n')
            x->line++;
        x->p++;
    }
}

static void xp_skip_ws(struct xp *x)
{
    while (*x->p && isspace((unsigned char)*x->p))
        xp_adv(x, 1);
}

/* append text [a, a+n) with entities decoded to *buf */
static void xp_text(char **buf, size_t *len, const char *a, size_t n)
{
    *buf = xgrow(*buf, *len + n + 1);
    for (size_t i = 0; i < n; i++) {
        if (a[i] != '&') {
            (*buf)[(*len)++] = a[i];
            continue;
        }
        const char *semi = memchr(a + i, ';', n - i);
        if (!semi) {
            (*buf)[(*len)++] = a[i];
            continue;
        }
        size_t el = (size_t)(semi - (a + i)) + 1;
        unsigned long c = 0;
        if (el == 5 && !strncmp(a + i, "&amp;", 5)) c = '&';
        else if (el == 4 && !strncmp(a + i, "&lt;", 4)) c = '<';
        else if (el == 4 && !strncmp(a + i, "&gt;", 4)) c = '>';
        else if (el == 6 && !strncmp(a + i, "&quot;", 6)) c = '"';
        else if (el == 6 && !strncmp(a + i, "&apos;", 6)) c = '\'';
        else if (a[i + 1] == '#')
            c = a[i + 2] == 'x' ? strtoul(a + i + 3, NULL, 16)
                                : strtoul(a + i + 2, NULL, 10);
        if (!c || c > 0x7f) {
            /* not ASCII: kept as written, it is only ever a description */
            (*buf)[(*len)++] = a[i];
            continue;
        }
        (*buf)[(*len)++] = (char)c;
        i += el - 1;
    }
    (*buf)[*len] = 0;
}

static struct xnode *xp_element(struct xp *x);

static void xp_misc(struct xp *x)
{
    for (;;) {
        xp_skip_ws(x);
        if (!strncmp(x->p, "<?", 2)) {
            const char *e = strstr(x->p, "?>");
            if (!e) die("line %d: an unclosed <?", x->line);
            xp_adv(x, (size_t)(e - x->p) + 2);
        } else if (!strncmp(x->p, "<!--", 4)) {
            const char *e = strstr(x->p, "-->");
            if (!e) die("line %d: an unclosed comment", x->line);
            xp_adv(x, (size_t)(e - x->p) + 3);
        } else if (!strncmp(x->p, "<!DOCTYPE", 9)) {
            const char *e = strchr(x->p, '>');
            if (!e) die("line %d: an unclosed DOCTYPE", x->line);
            xp_adv(x, (size_t)(e - x->p) + 1);
        } else {
            return;
        }
    }
}

static char *xp_name(struct xp *x)
{
    const char *a = x->p;
    while (*x->p && (isalnum((unsigned char)*x->p) || strchr("_:-.", *x->p)))
        xp_adv(x, 1);
    if (x->p == a)
        die("line %d: a name was expected, found '%.10s'", x->line, a);
    return xdup(a, (size_t)(x->p - a));
}

static struct xnode *xp_element(struct xp *x)
{
    if (*x->p != '<')
        die("line %d: an element was expected", x->line);
    struct xnode *n = xalloc(sizeof *n);
    n->line = x->line;
    xp_adv(x, 1);
    n->name = xp_name(x);
    for (;;) {
        xp_skip_ws(x);
        if (!strncmp(x->p, "/>", 2)) {
            xp_adv(x, 2);
            return n;
        }
        if (*x->p == '>') {
            xp_adv(x, 1);
            break;
        }
        char *an = xp_name(x);
        xp_skip_ws(x);
        if (*x->p != '=')
            die("line %d: attribute %s has no value", x->line, an);
        xp_adv(x, 1);
        xp_skip_ws(x);
        char q = *x->p;
        if (q != '"' && q != '\'')
            die("line %d: attribute %s's value is not quoted", x->line, an);
        xp_adv(x, 1);
        const char *a = x->p;
        while (*x->p && *x->p != q)
            xp_adv(x, 1);
        if (!*x->p)
            die("line %d: attribute %s's value is not closed", x->line, an);
        char *v = NULL;
        size_t vl = 0;
        xp_text(&v, &vl, a, (size_t)(x->p - a));
        xp_adv(x, 1);
        n->attr = xgrow(n->attr, (size_t)(n->nattr + 1) * sizeof *n->attr);
        n->attr[n->nattr].name = an;
        n->attr[n->nattr].value = v ? v : xdup("", 0);
        n->nattr++;
    }
    char *text = NULL;
    size_t tl = 0;
    for (;;) {
        if (!*x->p)
            die("line %d: <%s> is not closed", n->line, n->name);
        if (!strncmp(x->p, "</", 2)) {
            xp_adv(x, 2);
            char *cn = xp_name(x);
            if (strcmp(cn, n->name))
                die("line %d: </%s> closes <%s> of line %d", x->line, cn,
                    n->name, n->line);
            free(cn);
            xp_skip_ws(x);
            if (*x->p != '>')
                die("line %d: '>' was expected", x->line);
            xp_adv(x, 1);
            break;
        }
        if (!strncmp(x->p, "<!--", 4)) {
            const char *e = strstr(x->p, "-->");
            if (!e) die("line %d: an unclosed comment", x->line);
            xp_adv(x, (size_t)(e - x->p) + 3);
        } else if (!strncmp(x->p, "<![CDATA[", 9)) {
            const char *e = strstr(x->p, "]]>");
            if (!e) die("line %d: an unclosed CDATA", x->line);
            text = xgrow(text, tl + (size_t)(e - x->p) + 1);
            memcpy(text + tl, x->p + 9, (size_t)(e - x->p) - 9);
            tl += (size_t)(e - x->p) - 9;
            text[tl] = 0;
            xp_adv(x, (size_t)(e - x->p) + 3);
        } else if (!strncmp(x->p, "<?", 2)) {
            const char *e = strstr(x->p, "?>");
            if (!e) die("line %d: an unclosed <?", x->line);
            xp_adv(x, (size_t)(e - x->p) + 2);
        } else if (*x->p == '<') {
            struct xnode *k = xp_element(x);
            if (n->nkid == n->capkid) {
                n->capkid = n->capkid ? 2 * n->capkid : 4;
                n->kid = xgrow(n->kid, (size_t)n->capkid * sizeof *n->kid);
            }
            n->kid[n->nkid++] = k;
        } else {
            const char *a = x->p;
            while (*x->p && *x->p != '<')
                xp_adv(x, 1);
            xp_text(&text, &tl, a, (size_t)(x->p - a));
        }
    }
    /* trimmed, and runs of white space made one: SVD descriptions are
     * wrapped at whatever column the vendor's tool chose */
    if (text) {
        size_t o = 0;
        int sp = 0;
        for (size_t i = 0; i < tl; i++) {
            if (isspace((unsigned char)text[i])) {
                sp = o > 0;
                continue;
            }
            if (sp)
                text[o++] = ' ';
            sp = 0;
            text[o++] = text[i];
        }
        text[o] = 0;
    }
    n->text = text;
    return n;
}

static struct xnode *xml_parse(const char *s)
{
    struct xp x = { s, s, 1 };
    xp_misc(&x);
    struct xnode *root = xp_element(&x);
    xp_misc(&x);
    xp_skip_ws(&x);
    if (*x.p)
        die("line %d: text after the document's end", x.line);
    return root;
}

static struct xnode *kid(const struct xnode *n, const char *name)
{
    for (int i = 0; n && i < n->nkid; i++)
        if (!strcmp(n->kid[i]->name, name))
            return n->kid[i];
    return NULL;
}

static const char *kidtext(const struct xnode *n, const char *name)
{
    const struct xnode *k = kid(n, name);
    return k && k->text ? k->text : NULL;
}

static const char *attr(const struct xnode *n, const char *name)
{
    for (int i = 0; n && i < n->nattr; i++)
        if (!strcmp(n->attr[i].name, name))
            return n->attr[i].value;
    return NULL;
}

/* SVD's scaledNonNegativeInteger: decimal, 0x hex, #binary, with an
 * optional k/M/G multiplier */
static unsigned long long svd_num(const char *s, const char *what, int line)
{
    if (!s)
        die("line %d: %s is missing", line, what);
    while (isspace((unsigned char)*s))
        s++;
    unsigned long long v = 0;
    char *e;
    if (*s == '#') {
        for (s++; *s == '0' || *s == '1'; s++)
            v = v * 2 + (unsigned)(*s - '0');
        e = (char *)s;
    } else {
        v = strtoull(s, &e, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                                 ? 16 : 10);
        if (e == s)
            die("line %d: %s '%s' is not a number", line, what, s);
    }
    switch (*e) {
    case 'k': case 'K': v <<= 10; e++; break;
    case 'm': case 'M': v <<= 20; e++; break;
    case 'g': case 'G': v <<= 30; e++; break;
    default: break;
    }
    while (isspace((unsigned char)*e))
        e++;
    if (*e)
        die("line %d: %s '%s' is not a number", line, what, s);
    return v;
}

/* ---- the device ------------------------------------------------------ */

struct field { const char *name, *desc; int lsb, width; };
struct reg {
    char *name;
    const char *desc;
    unsigned long long off;
    int size;                   /* bits */
    int access;                 /* 0 rw, 1 read-only, 2 write-only */
    unsigned long long reset;
    struct field *f;
    int nf;
    const char *alt;            /* alternateRegister / alternateGroup */
    int line;
};
struct irq { const char *name, *desc; int value; };
struct periph {
    const char *name, *desc, *group, *derived;
    unsigned long long base;
    struct reg *r;
    int nr;
    struct periph *layout;      /* whose registers these are */
    struct xnode *node;
};
struct device {
    const char *name, *cpu, *cpurev, *desc;
    int prio_bits, fpu, mpu, vendor_systick;
    struct periph *p;
    int np;
    struct irq *irq;
    int nirq;
};

static int access_of(const char *a)
{
    if (!a) return -1;
    if (!strcmp(a, "read-only")) return 1;
    if (!strcmp(a, "write-only") || !strcmp(a, "writeOnce")) return 2;
    return 0;
}

/* The fields of <register>, by bitOffset/bitWidth, lsb/msb or
 * bitRange [msb:lsb]. */
static void read_fields(struct reg *r, const struct xnode *rn)
{
    const struct xnode *fs = kid(rn, "fields");
    for (int i = 0; fs && i < fs->nkid; i++) {
        const struct xnode *f = fs->kid[i];
        if (strcmp(f->name, "field"))
            continue;
        if (attr(f, "derivedFrom"))
            die("line %d: a field derivedFrom another is not supported yet",
                f->line);
        struct field fd;
        memset(&fd, 0, sizeof fd);
        fd.name = kidtext(f, "name");
        fd.desc = kidtext(f, "description");
        if (!fd.name)
            die("line %d: a field without a name", f->line);
        if (kidtext(f, "bitOffset")) {
            fd.lsb = (int)svd_num(kidtext(f, "bitOffset"), "bitOffset", f->line);
            fd.width = (int)svd_num(kidtext(f, "bitWidth"), "bitWidth", f->line);
        } else if (kidtext(f, "lsb")) {
            fd.lsb = (int)svd_num(kidtext(f, "lsb"), "lsb", f->line);
            fd.width = (int)svd_num(kidtext(f, "msb"), "msb", f->line) - fd.lsb + 1;
        } else if (kidtext(f, "bitRange")) {
            unsigned msb, lsb;
            if (sscanf(kidtext(f, "bitRange"), " [%u:%u]", &msb, &lsb) != 2)
                die("line %d: bitRange '%s'", f->line, kidtext(f, "bitRange"));
            fd.lsb = (int)lsb;
            fd.width = (int)msb - (int)lsb + 1;
        } else {
            die("line %d: field %s has no bit position", f->line, fd.name);
        }
        if (fd.width < 1 || fd.lsb < 0 || fd.lsb + fd.width > r->size)
            die("line %d: field %s.%s, bits %d..%d, is outside the %d-bit "
                "register", f->line, r->name, fd.name, fd.lsb,
                fd.lsb + fd.width - 1, r->size);
        r->f = xgrow(r->f, (size_t)(r->nf + 1) * sizeof *r->f);
        r->f[r->nf++] = fd;
    }
}

static void push_reg(struct periph *p, struct reg *r)
{
    p->r = xgrow(p->r, (size_t)(p->nr + 1) * sizeof *p->r);
    p->r[p->nr++] = *r;
}

/* a register, or the N a dim array makes: NAME%s gives NAME0..NAMEn (or
 * the dimIndex names), NAME[%s] an array, written as NAME[n] */
static void read_register(struct periph *p, const struct xnode *rn,
                          int dsize, int daccess, unsigned long long dreset)
{
    struct reg r;
    memset(&r, 0, sizeof r);
    r.line = rn->line;
    const char *name = kidtext(rn, "name");
    if (!name)
        die("line %d: a register without a name", rn->line);
    if (attr(rn, "derivedFrom"))
        die("line %d: register %s is derivedFrom another, which is not "
            "supported yet", rn->line, name);
    r.desc = kidtext(rn, "description");
    r.off = svd_num(kidtext(rn, "addressOffset"), "addressOffset", rn->line);
    r.size = kidtext(rn, "size") ? (int)svd_num(kidtext(rn, "size"), "size",
                                                rn->line) : dsize;
    if (r.size != 8 && r.size != 16 && r.size != 32 && r.size != 64)
        die("line %d: register %s is %d bits wide", rn->line, name, r.size);
    r.access = access_of(kidtext(rn, "access"));
    if (r.access < 0)
        r.access = daccess;
    r.reset = kidtext(rn, "resetValue")
        ? svd_num(kidtext(rn, "resetValue"), "resetValue", rn->line) : dreset;
    r.alt = kidtext(rn, "alternateRegister");
    if (!r.alt)
        r.alt = kidtext(rn, "alternateGroup");
    read_fields(&r, rn);
    const char *dim = kidtext(rn, "dim");
    if (!dim) {
        r.name = xdup(name, strlen(name));
        push_reg(p, &r);
        return;
    }
    int n = (int)svd_num(dim, "dim", rn->line);
    unsigned long long inc = svd_num(kidtext(rn, "dimIncrement"),
                                     "dimIncrement", rn->line);
    const char *pct = strstr(name, "%s");
    if (!pct)
        die("line %d: register array %s has no %%s in its name", rn->line,
            name);
    int bracket = pct > name && pct[-1] == '[' && pct[2] == ']';
    const char *idx = kidtext(rn, "dimIndex");
    for (int k = 0; k < n; k++) {
        char ix[64];
        if (bracket || !idx) {
            snprintf(ix, sizeof ix, "%d", k);
        } else if (strchr(idx, '-') && !strchr(idx, ',')) {
            int lo = 0, hi = 0;
            if (sscanf(idx, "%d-%d", &lo, &hi) == 2)
                snprintf(ix, sizeof ix, "%d", lo + k);
            else if (isalpha((unsigned char)idx[0]))
                snprintf(ix, sizeof ix, "%c", idx[0] + k);
            else
                die("line %d: dimIndex '%s'", rn->line, idx);
        } else {
            const char *q = idx;
            for (int j = 0; j < k && q; j++) {
                q = strchr(q, ',');
                if (q) q++;
            }
            if (!q)
                die("line %d: dimIndex '%s' names fewer than %d", rn->line,
                    idx, n);
            size_t l = strcspn(q, ",");
            snprintf(ix, sizeof ix, "%.*s", (int)l, q);
        }
        size_t pre = (size_t)(pct - name);
        char buf[256];
        snprintf(buf, sizeof buf, "%.*s%s%s", (int)pre, name, ix, pct + 2);
        struct reg c = r;
        c.name = xdup(buf, strlen(buf));
        c.off = r.off + (unsigned long long)k * inc;
        push_reg(p, &c);
    }
}

static struct device *read_device(struct xnode *root)
{
    if (strcmp(root->name, "device"))
        die("the document is <%s>, not an SVD <device>", root->name);
    struct device *d = xalloc(sizeof *d);
    d->name = kidtext(root, "name");
    d->desc = kidtext(root, "description");
    if (!d->name)
        die("the device has no name");
    const struct xnode *cpu = kid(root, "cpu");
    d->cpu = cpu ? kidtext(cpu, "name") : NULL;
    d->cpurev = cpu ? kidtext(cpu, "revision") : NULL;
    if (cpu) {
        d->prio_bits = kidtext(cpu, "nvicPrioBits")
            ? (int)svd_num(kidtext(cpu, "nvicPrioBits"), "nvicPrioBits",
                           cpu->line) : 0;
        d->fpu = kidtext(cpu, "fpuPresent") &&
                 !strcmp(kidtext(cpu, "fpuPresent"), "true");
        d->mpu = kidtext(cpu, "mpuPresent") &&
                 !strcmp(kidtext(cpu, "mpuPresent"), "true");
        d->vendor_systick = kidtext(cpu, "vendorSystickConfig") &&
                            !strcmp(kidtext(cpu, "vendorSystickConfig"), "true");
    }
    int dsize = kidtext(root, "size") ? (int)svd_num(kidtext(root, "size"),
                                                     "size", root->line) : 32;
    int dacc = access_of(kidtext(root, "access"));
    if (dacc < 0) dacc = 0;
    unsigned long long dreset = kidtext(root, "resetValue")
        ? svd_num(kidtext(root, "resetValue"), "resetValue", root->line) : 0;

    const struct xnode *ps = kid(root, "peripherals");
    if (!ps)
        die("the device has no <peripherals>");
    d->p = xalloc((size_t)(ps->nkid + 1) * sizeof *d->p);
    for (int i = 0; i < ps->nkid; i++) {
        struct xnode *pn = ps->kid[i];
        if (strcmp(pn->name, "peripheral"))
            continue;
        struct periph *p = &d->p[d->np++];
        p->node = pn;
        p->name = kidtext(pn, "name");
        if (!p->name)
            die("line %d: a peripheral without a name", pn->line);
        p->desc = kidtext(pn, "description");
        p->group = kidtext(pn, "groupName");
        p->derived = attr(pn, "derivedFrom");
        p->base = svd_num(kidtext(pn, "baseAddress"), "baseAddress", pn->line);
        for (int k = 0; k < pn->nkid; k++)
            if (!strcmp(pn->kid[k]->name, "interrupt")) {
                const struct xnode *in = pn->kid[k];
                struct irq q;
                q.name = kidtext(in, "name");
                q.desc = kidtext(in, "description");
                q.value = (int)svd_num(kidtext(in, "value"), "value", in->line);
                int dup = 0;
                for (int j = 0; j < d->nirq; j++)
                    if (!strcmp(d->irq[j].name, q.name)) {
                        if (d->irq[j].value != q.value)
                            die("line %d: interrupt %s is %d here and %d "
                                "before", in->line, q.name, q.value,
                                d->irq[j].value);
                        dup = 1;
                    }
                if (!dup) {
                    d->irq = xgrow(d->irq, (size_t)(d->nirq + 1) * sizeof *d->irq);
                    d->irq[d->nirq++] = q;
                }
            }
        const struct xnode *rs = kid(pn, "registers");
        int psize = kidtext(pn, "size") ? (int)svd_num(kidtext(pn, "size"),
                                                       "size", pn->line) : dsize;
        int pacc = access_of(kidtext(pn, "access"));
        if (pacc < 0) pacc = dacc;
        unsigned long long preset = kidtext(pn, "resetValue")
            ? svd_num(kidtext(pn, "resetValue"), "resetValue", pn->line)
            : dreset;
        for (int k = 0; rs && k < rs->nkid; k++) {
            if (!strcmp(rs->kid[k]->name, "cluster"))
                die("line %d: peripheral %s has a <cluster>, which is not "
                    "supported yet", rs->kid[k]->line, p->name);
            if (!strcmp(rs->kid[k]->name, "register"))
                read_register(p, rs->kid[k], psize, pacc, preset);
        }
    }
    /* derivedFrom: the registers, and what else was not said, are the
     * base's; a chain is followed to its end */
    for (int i = 0; i < d->np; i++) {
        struct periph *p = &d->p[i];
        struct periph *b = p;
        for (int hops = 0; b->derived; hops++) {
            if (b->nr)
                die("line %d: %s is derivedFrom %s and has registers of its "
                    "own; merging the two is not supported yet",
                    b->node->line, b->name, b->derived);
            struct periph *nb = NULL;
            for (int j = 0; j < d->np; j++)
                if (!strcmp(d->p[j].name, b->derived))
                    nb = &d->p[j];
            if (!nb)
                die("line %d: %s is derivedFrom %s, which is not in the file",
                    b->node->line, b->name, b->derived);
            if (hops > d->np)
                die("%s: derivedFrom goes round in a circle", p->name);
            b = nb;
        }
        p->layout = b;
        if (!p->desc) p->desc = p->layout->desc;
        if (!p->group) p->group = p->layout->group;
    }
    for (int i = 0; i < d->np; i++) {
        struct periph *p = &d->p[i];
        if (p->layout == p && !p->nr)
            fprintf(stderr, "embsvd: %s: warning: peripheral %s has no "
                    "registers\n", g_file, p->name);
    }
    return d;
}

/* ---- the header -------------------------------------------------------- */

static int reg_cmp(const void *a, const void *b)
{
    const struct reg *x = a, *y = b;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return x->line - y->line;
}

static const char *ctype_of(int bits)
{
    return bits == 8 ? "uint8_t" : bits == 16 ? "uint16_t"
         : bits == 64 ? "uint64_t" : "uint32_t";
}

static const char *qual_of(int access)
{
    return access == 1 ? "__I " : access == 2 ? "__O " : "__IO";
}

/* a C identifier from an SVD name: what is not alphanumeric becomes _ */
static const char *ident(const char *s)
{
    static char buf[4][256];
    static int at;
    char *o = buf[at++ & 3];
    size_t i = 0;
    for (; s[i] && i < 255; i++)
        o[i] = isalnum((unsigned char)s[i]) ? s[i] : '_';
    o[i] = 0;
    return o;
}

static void comment(FILE *f, const char *s)
{
    if (!s)
        return;
    fputs("  /*!< ", f);
    for (; *s; s++)
        if (!(s[0] == '*' && s[1] == '/'))
            fputc(*s, f);
    fputs(" */", f);
}

struct core_exc { int n; const char *irq, *handler; int m0; };
static const struct core_exc core_excs[] = {
    { -14, "NonMaskableInt", "NMI_Handler", 1 },
    { -13, "HardFault", "HardFault_Handler", 1 },
    { -12, "MemoryManagement", "MemManage_Handler", 0 },
    { -11, "BusFault", "BusFault_Handler", 0 },
    { -10, "UsageFault", "UsageFault_Handler", 0 },
    { -5, "SVCall", "SVC_Handler", 1 },
    { -4, "DebugMonitor", "DebugMon_Handler", 0 },
    { -2, "PendSV", "PendSV_Handler", 1 },
    { -1, "SysTick", "SysTick_Handler", 1 },
};

/* ARMv6-M and ARMv8-M Baseline have no MemManage, BusFault, UsageFault
 * or DebugMonitor exception */
static int cpu_baseline(const struct device *d)
{
    return d->cpu && (!strncmp(d->cpu, "CM0", 3) || !strcmp(d->cpu, "CM1") ||
                      !strcmp(d->cpu, "CM23"));
}

/* CMSIS's core header for the SVD's cpu name */
static const char *core_header(const struct device *d)
{
    static const struct { const char *svd, *h, *rev; } m[] = {
        { "CM0", "core_cm0.h", "__CM0_REV" },
        { "CM0PLUS", "core_cm0plus.h", "__CM0PLUS_REV" },
        { "CM0+", "core_cm0plus.h", "__CM0PLUS_REV" },
        { "CM1", "core_cm1.h", "__CM1_REV" },
        { "CM3", "core_cm3.h", "__CM3_REV" },
        { "CM4", "core_cm4.h", "__CM4_REV" },
        { "CM7", "core_cm7.h", "__CM7_REV" },
        { "CM23", "core_cm23.h", "__CM23_REV" },
        { "CM33", "core_cm33.h", "__CM33_REV" },
        { "CM55", "core_cm55.h", "__CM55_REV" },
        { "CM85", "core_cm85.h", "__CM85_REV" },
    };
    for (size_t i = 0; d->cpu && i < sizeof m / sizeof m[0]; i++)
        if (!strcmp(d->cpu, m[i].svd))
            return m[i].h;
    return NULL;
}

static const char *core_rev_macro(const struct device *d)
{
    static char buf[32];
    const char *h = core_header(d);
    if (!h)
        return NULL;
    /* core_cm4.h -> __CM4_REV */
    snprintf(buf, sizeof buf, "__%.*s_REV", (int)(strlen(h) - 7), h + 5);
    for (char *q = buf; *q; q++)
        *q = (char)toupper((unsigned char)*q);
    return buf;
}

static unsigned cpu_rev_value(const char *rev)
{
    unsigned r = 0, p = 0;
    if (rev && sscanf(rev, "r%up%u", &r, &p) == 2)
        return r << 8 | p;
    return 0;
}

/* The ARM-defined part of the private peripheral bus -- ITM, DWT, FPB,
 * the System Control Space (NVIC, SCB, SysTick, MPU, FPU), TPIU, ETM --
 * which CMSIS-Core describes. With it included, a vendor's SVD copy of
 * them would define NVIC_Type and SCB_HFSR_FORCED_Msk a second time, so
 * they are left out, as svdconv leaves them out. 0xE0042000 and up is the
 * vendor's own (ST's DBGMCU) and stays. */
static int cmsis_core_periph(const struct periph *p)
{
    return p->base >= 0xE0000000ULL && p->base < 0xE0042000ULL;
}

static int irq_cmp(const void *a, const void *b)
{
    return ((const struct irq *)a)->value - ((const struct irq *)b)->value;
}

static void write_struct(FILE *f, const struct periph *p)
{
    struct reg *r = xalloc((size_t)p->nr * sizeof *r);
    memcpy(r, p->r, (size_t)p->nr * sizeof *r);
    qsort(r, (size_t)p->nr, sizeof *r, reg_cmp);
    fprintf(f, "/* %s%s%s */\ntypedef struct {\n", p->name,
            p->desc ? ": " : "", p->desc ? p->desc : "");
    unsigned long long at = 0;
    int nres = 0;
    for (int i = 0; i < p->nr; ) {
        /* the registers that share this offset are a union */
        int j = i + 1;
        while (j < p->nr && r[j].off == r[i].off)
            j++;
        if (r[i].off < at)
            die("line %d: %s.%s at offset 0x%llx overlaps the register "
                "before it", r[i].line, p->name, r[i].name, r[i].off);
        if (r[i].off > at)
            fprintf(f, "  uint8_t RESERVED%d[%llu];\n", nres++,
                    r[i].off - at);
        /* SVD says which register is another's alternate view; two at
         * one offset that do not say so are a mistake in the file */
        for (int k = i + 1; k < j; k++)
            if (!r[k].alt && !r[i].alt)
                die("line %d: %s.%s at offset 0x%llx overlaps %s, and "
                    "neither is the other's alternateRegister", r[k].line,
                    p->name, r[k].name, r[k].off, r[i].name);
        unsigned long long end = r[i].off;
        if (j - i > 1)
            fprintf(f, "  union {\n");
        for (int k = i; k < j; k++) {
            const char *nm = r[k].name;
            const char *br = strchr(nm, '[');
            fprintf(f, "%s  %s %s %s;", j - i > 1 ? "  " : "",
                    qual_of(r[k].access), ctype_of(r[k].size), ident(nm));
            if (br)
                die("line %d: register %s: an array written NAME[%%s] is "
                    "not supported yet", r[k].line, nm);
            fprintf(f, "  /*!< 0x%03llx", r[k].off);
            if (r[k].desc) {
                fputs(": ", f);
                for (const char *s = r[k].desc; *s; s++)
                    if (!(s[0] == '*' && s[1] == '/'))
                        fputc(*s, f);
            }
            fputs(" */\n", f);
            if (r[k].off + (unsigned)r[k].size / 8 > end)
                end = r[k].off + (unsigned)r[k].size / 8;
        }
        if (j - i > 1)
            fprintf(f, "  };\n");
        at = end;
        i = j;
    }
    fprintf(f, "} %s_Type;\n\n", ident(p->name));
    free(r);
}

static void write_header(FILE *f, const struct device *d, int cmsis)
{
    char guard[256];
    snprintf(guard, sizeof guard, "%s_H", ident(d->name));
    for (char *q = guard; *q; q++)
        *q = (char)toupper((unsigned char)*q);
    fprintf(f, "/* %s: generated by embsvd from %s. Do not edit.\n", d->name,
            g_file);
    if (d->desc)
        fprintf(f, " * %s\n", d->desc);
    fprintf(f, " */\n#ifndef %s\n#define %s\n\n#include <stdint.h>\n\n",
            guard, guard);
    fprintf(f, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");

    /* the interrupt numbers */
    struct irq *q = xalloc((size_t)(d->nirq + 1) * sizeof *q);
    memcpy(q, d->irq, (size_t)d->nirq * sizeof *q);
    qsort(q, (size_t)d->nirq, sizeof *q, irq_cmp);
    fprintf(f, "typedef enum {\n");
    int base = cpu_baseline(d);
    for (size_t i = 0; i < sizeof core_excs / sizeof core_excs[0]; i++)
        if (!base || core_excs[i].m0)
            fprintf(f, "  %s_IRQn = %d,\n", core_excs[i].irq, core_excs[i].n);
    for (int i = 0; i < d->nirq; i++) {
        fprintf(f, "  %s_IRQn = %d,", ident(q[i].name), q[i].value);
        comment(f, q[i].desc);
        fputc('\n', f);
    }
    fprintf(f, "} IRQn_Type;\n\n");
    free(q);

    if (cmsis) {
        const char *h = core_header(d);
        if (!h)
            die("cpu '%s' has no CMSIS core header that embsvd knows; "
                "--no-cmsis writes the header without one",
                d->cpu ? d->cpu : "(none)");
        fprintf(f, "#define %s 0x%04xU\n", core_rev_macro(d),
                cpu_rev_value(d->cpurev));
        fprintf(f, "#define __MPU_PRESENT %d\n", d->mpu);
        fprintf(f, "#define __FPU_PRESENT %d\n", d->fpu);
        fprintf(f, "#define __NVIC_PRIO_BITS %d\n", d->prio_bits);
        fprintf(f, "#define __Vendor_SysTickConfig %d\n\n", d->vendor_systick);
        fprintf(f, "#include \"%s\"\n\n", h);
    } else {
        fprintf(f, "#ifndef __I\n#define __I volatile const\n#endif\n"
                   "#ifndef __O\n#define __O volatile\n#endif\n"
                   "#ifndef __IO\n#define __IO volatile\n#endif\n\n");
    }

    if (cmsis) {
        int any = 0;
        for (int i = 0; i < d->np; i++)
            if (cmsis_core_periph(&d->p[i]))
                fprintf(f, "%s%s", any++ ? ", " : "/* CMSIS-Core describes "
                        "these, so the SVD's copies are left out: ",
                        d->p[i].name);
        if (any)
            fprintf(f, " */\n\n");
    }
#define SKIP(p) (cmsis && cmsis_core_periph(p))
    /* one struct per layout */
    for (int i = 0; i < d->np; i++)
        if (d->p[i].layout == &d->p[i] && d->p[i].nr && !SKIP(&d->p[i]))
            write_struct(f, &d->p[i]);

    /* where each one is */
    for (int i = 0; i < d->np; i++)
        if (!SKIP(&d->p[i]))
            fprintf(f, "#define %s_BASE 0x%08llxUL\n", ident(d->p[i].name),
                    d->p[i].base);
    fputc('\n', f);
    for (int i = 0; i < d->np; i++)
        if (d->p[i].layout->nr && !SKIP(&d->p[i]) && !SKIP(d->p[i].layout))
            fprintf(f, "#define %s ((%s_Type *)%s_BASE)\n",
                    ident(d->p[i].name), ident(d->p[i].layout->name),
                    ident(d->p[i].name));
    fputc('\n', f);

    /* every field: LAYOUT_REG_FIELD_Pos and _Msk */
    for (int i = 0; i < d->np; i++) {
        const struct periph *p = &d->p[i];
        if (p->layout != p || SKIP(p))
            continue;
        for (int k = 0; k < p->nr; k++) {
            const struct reg *r = &p->r[k];
            for (int m = 0; m < r->nf; m++) {
                const struct field *fd = &r->f[m];
                char pre[512];
                snprintf(pre, sizeof pre, "%s_%s_%s", ident(p->name),
                         ident(r->name), ident(fd->name));
                unsigned long long mask = fd->width >= 64 ? ~0ULL
                    : ((1ULL << fd->width) - 1);
                fprintf(f, "#define %s_Pos %dU\n", pre, fd->lsb);
                fprintf(f, "#define %s_Msk (0x%llxUL << %s_Pos)\n", pre, mask,
                        pre);
            }
        }
    }
#undef SKIP
    fprintf(f, "\n#ifdef __cplusplus\n}\n#endif\n\n#endif /* %s */\n", guard);
}

/* ---- the startup --------------------------------------------------------- */

static void write_startup(FILE *f, const struct device *d)
{
    int base = cpu_baseline(d), max = -1;
    for (int i = 0; i < d->nirq; i++)
        if (d->irq[i].value > max)
            max = d->irq[i].value;
    fprintf(f, "/* %s startup: generated by embsvd from %s.\n"
            " *\n"
            " * The vector table, with a weak handler for every exception and\n"
            " * interrupt (define one with the same name to take it), and a\n"
            " * Reset_Handler that copies .data from flash, zeroes .bss, calls\n"
            " * SystemInit if the program has one, runs the constructors and\n"
            " * calls main -- by the symbols the generated linker script\n"
            " * defines. */\n", d->name, g_file);
    fprintf(f, "extern unsigned long _sidata[], _sdata[], _edata[], _sbss[], "
               "_ebss[], _estack[];\n"
               "extern void (*__init_array_start[])(void);\n"
               "extern void (*__init_array_end[])(void);\n"
               "int main(void);\n"
               "void Reset_Handler(void);\n\n"
               "void Default_Handler(void)\n{\n    for (;;)\n        ;\n}\n\n"
               "__attribute__((weak)) void SystemInit(void)\n{\n}\n\n");
    for (size_t i = 0; i < sizeof core_excs / sizeof core_excs[0]; i++)
        if (!base || core_excs[i].m0)
            fprintf(f, "void %s(void) __attribute__((weak, alias(\"Default_Handler\")));\n",
                    core_excs[i].handler);
    for (int i = 0; i < d->nirq; i++)
        fprintf(f, "void %s_IRQHandler(void) __attribute__((weak, alias(\"Default_Handler\")));\n",
                ident(d->irq[i].name));
    fprintf(f, "\n__attribute__((section(\".isr_vector\"), used))\n"
               "void (*const g_pfnVectors[%d])(void) = {\n"
               "    (void (*)(void))_estack,\n    Reset_Handler,\n", 16 + max + 1);
    static const char *const slots[14] = {
        "NMI_Handler", "HardFault_Handler", "MemManage_Handler",
        "BusFault_Handler", "UsageFault_Handler", 0, 0, 0, 0, "SVC_Handler",
        "DebugMon_Handler", 0, "PendSV_Handler", "SysTick_Handler" };
    for (int i = 0; i < 14; i++) {
        const char *s = slots[i];
        if (s && base && (i == 2 || i == 3 || i == 4 || i == 10))
            s = NULL;
        fprintf(f, "    %s,\n", s ? s : "0");
    }
    for (int v = 0; v <= max; v++) {
        const char *s = NULL;
        for (int i = 0; i < d->nirq; i++)
            if (d->irq[i].value == v)
                s = d->irq[i].name;
        if (s)
            fprintf(f, "    %s_IRQHandler,         /* %d */\n", ident(s), v);
        else
            fprintf(f, "    0,                     /* %d */\n", v);
    }
    fprintf(f, "};\n\n"
               "void Reset_Handler(void)\n{\n"
               "    unsigned long *src = _sidata, *dst = _sdata;\n"
               "    while (dst < _edata)\n        *dst++ = *src++;\n"
               "    for (dst = _sbss; dst < _ebss; )\n        *dst++ = 0;\n"
               "    SystemInit();\n"
               "    for (void (**p)(void) = __init_array_start; p < __init_array_end; p++)\n"
               "        (*p)();\n"
               "    main();\n"
               "    for (;;)\n        ;\n}\n");
}

/* ---- the linker script ------------------------------------------------------ */

static void parse_region(const char *s, unsigned long long *org,
                         unsigned long long *len, const char *what)
{
    const char *c = strchr(s, ':');
    if (!c)
        die("%s wants ORIGIN:LENGTH, as in 0x08000000:1M", what);
    char a[64];
    snprintf(a, sizeof a, "%.*s", (int)(c - s), s);
    *org = svd_num(a, what, 0);
    *len = svd_num(c + 1, what, 0);
}

static void write_ld(FILE *f, const struct device *d, const char *flash,
                     const char *ram)
{
    unsigned long long fo, fl, ro, rl;
    parse_region(flash, &fo, &fl, "--flash");
    parse_region(ram, &ro, &rl, "--ram");
    fprintf(f, "/* %s: generated by embsvd, in STM32CubeMX's shape. */\n"
               "ENTRY(Reset_Handler)\n"
               "_estack = ORIGIN(RAM) + LENGTH(RAM);\n\n"
               "MEMORY\n{\n"
               "  FLASH (rx)  : ORIGIN = 0x%08llx, LENGTH = 0x%llx\n"
               "  RAM   (xrw) : ORIGIN = 0x%08llx, LENGTH = 0x%llx\n}\n\n",
            d->name, fo, fl, ro, rl);
    fputs("SECTIONS\n{\n"
          "  .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) . = ALIGN(4); } >FLASH\n"
          "  .text : { . = ALIGN(4); *(.text) *(.text*) KEEP(*(.init)) KEEP(*(.fini))\n"
          "            . = ALIGN(4); _etext = .; } >FLASH\n"
          "  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH\n"
          "  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH\n"
          "  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .);\n"
          "                     KEEP(*(.preinit_array*))\n"
          "                     PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH\n"
          "  .init_array : { PROVIDE_HIDDEN(__init_array_start = .);\n"
          "                  KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*))\n"
          "                  PROVIDE_HIDDEN(__init_array_end = .); } >FLASH\n"
          "  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .);\n"
          "                  KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*))\n"
          "                  PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH\n"
          "  _sidata = LOADADDR(.data);\n"
          "  .data : { . = ALIGN(4); _sdata = .; *(.data) *(.data*) . = ALIGN(4);\n"
          "            _edata = .; } >RAM AT> FLASH\n"
          "  .bss : { . = ALIGN(4); _sbss = .; __bss_start__ = _sbss; *(.bss) *(.bss*)\n"
          "           *(COMMON) . = ALIGN(4); _ebss = .; __bss_end__ = _ebss; } >RAM\n"
          "  PROVIDE(end = _ebss);\n"
          "  PROVIDE(_end = _ebss);\n"
          "}\n", f);
}

/* ---- listing ----------------------------------------------------------------- */

static void list_device(const struct device *d)
{
    printf("%s: %s, %d peripherals, %d interrupts\n", d->name,
           d->cpu ? d->cpu : "(no cpu)", d->np, d->nirq);
    for (int i = 0; i < d->np; i++) {
        const struct periph *p = &d->p[i];
        printf("  %-12s 0x%08llx", p->name, p->base);
        if (p->layout != p)
            printf("  like %s", p->layout->name);
        else
            printf("  %d registers", p->nr);
        if (p->desc)
            printf("  %s", p->desc);
        putchar('\n');
    }
}

static void show_periph(const struct device *d, const char *name)
{
    const struct periph *p = NULL;
    for (int i = 0; i < d->np; i++)
        if (!strcmp(d->p[i].name, name))
            p = &d->p[i];
    if (!p)
        die("there is no peripheral %s (--list names them)", name);
    const struct periph *l = p->layout;
    printf("%s at 0x%08llx%s%s\n", p->name, p->base, p->desc ? ": " : "",
           p->desc ? p->desc : "");
    struct reg *r = xalloc((size_t)l->nr * sizeof *r);
    memcpy(r, l->r, (size_t)l->nr * sizeof *r);
    qsort(r, (size_t)l->nr, sizeof *r, reg_cmp);
    for (int k = 0; k < l->nr; k++) {
        printf("  0x%08llx  +0x%03llx  %-14s %2d bits  %s  reset 0x%llx%s%s\n",
               p->base + r[k].off, r[k].off, r[k].name, r[k].size,
               r[k].access == 1 ? "ro" : r[k].access == 2 ? "wo" : "rw",
               r[k].reset, r[k].desc ? "  " : "", r[k].desc ? r[k].desc : "");
        for (int m = 0; m < r[k].nf; m++) {
            const struct field *fd = &r[k].f[m];
            if (fd->width == 1)
                printf("      [%d]     %s\n", fd->lsb, fd->name);
            else
                printf("      [%d:%d]%*s%s\n", fd->lsb + fd->width - 1,
                       fd->lsb, fd->lsb + fd->width - 1 >= 10 ? 1 : 2, "",
                       fd->name);
        }
    }
    free(r);
}

/* ---- main ---------------------------------------------------------------- */

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open it");
    size_t cap = 1 << 20, n = 0;
    char *b = xalloc(cap);
    for (;;) {
        if (n + 65536 > cap)
            b = xgrow(b, cap *= 2);
        size_t got = fread(b + n, 1, cap - n - 1, f);
        n += got;
        if (got == 0)
            break;
    }
    fclose(f);
    b[n] = 0;
    if (strlen(b) != n)
        die("it has a NUL byte in it: not an SVD file");
    return b;
}

static FILE *open_out(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "embsvd: cannot write '%s'\n", path);
        exit(1);
    }
    return f;
}

static void close_out(FILE *f, const char *path)
{
    if (fclose(f) != 0) {
        fprintf(stderr, "embsvd: cannot write '%s'\n", path);
        exit(1);
    }
}

static void usage(void)
{
    fprintf(stderr,
        "usage: embsvd DEVICE.svd [--header FILE] [--no-cmsis]\n"
        "              [--nvic-prio-bits N] [--fpu-present 0|1]\n"
        "              [--startup FILE]\n"
        "              [--ld FILE --flash ORIGIN:LENGTH --ram ORIGIN:LENGTH]\n"
        "       embsvd DEVICE.svd --list | --show PERIPHERAL\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *hdr = NULL, *st = NULL, *ld = NULL;
    const char *flash = NULL, *ram = NULL, *show = NULL;
    const char *prio = NULL, *fpu = NULL;
    int cmsis = 1, list = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char **slot = !strcmp(a, "--header") ? &hdr
                          : !strcmp(a, "--startup") ? &st
                          : !strcmp(a, "--ld") ? &ld
                          : !strcmp(a, "--flash") ? &flash
                          : !strcmp(a, "--ram") ? &ram
                          : !strcmp(a, "--show") ? &show
                          : !strcmp(a, "--nvic-prio-bits") ? &prio
                          : !strcmp(a, "--fpu-present") ? &fpu : NULL;
        if (slot) {
            if (++i == argc) {
                fprintf(stderr, "embsvd: %s needs a value\n", a);
                return 2;
            }
            *slot = argv[i];
        } else if (!strcmp(a, "--no-cmsis")) {
            cmsis = 0;
        } else if (!strcmp(a, "--list")) {
            list = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "embsvd: unknown option '%s'\n", a);
            usage();
        } else if (in) {
            fprintf(stderr, "embsvd: one SVD file at a time\n");
            return 2;
        } else {
            in = a;
        }
    }
    if (!in || (!hdr && !st && !ld && !list && !show))
        usage();
    if (ld && (!flash || !ram)) {
        fprintf(stderr, "embsvd: --ld needs --flash ORIGIN:LENGTH and "
                        "--ram ORIGIN:LENGTH: an SVD describes the "
                        "peripherals, not the memories\n");
        return 2;
    }
    g_file = in;
    char *text = read_all(in);
    struct device *d = read_device(xml_parse(text));
    if (prio) {
        int n = atoi(prio);
        if (n < 2 || n > 8) {
            fprintf(stderr, "embsvd: --nvic-prio-bits is 2 to 8\n");
            return 2;
        }
        d->prio_bits = n;
    }
    if (fpu) {
        if (strcmp(fpu, "0") && strcmp(fpu, "1")) {
            fprintf(stderr, "embsvd: --fpu-present is 0 or 1\n");
            return 2;
        }
        d->fpu = fpu[0] == '1';
    }
    if (list)
        list_device(d);
    if (show)
        show_periph(d, show);
    if (hdr) {
        FILE *f = open_out(hdr);
        write_header(f, d, cmsis);
        close_out(f, hdr);
    }
    if (st) {
        FILE *f = open_out(st);
        write_startup(f, d);
        close_out(f, st);
    }
    if (ld) {
        FILE *f = open_out(ld);
        write_ld(f, d, flash, ram);
        close_out(f, ld);
    }
    return 0;
}
