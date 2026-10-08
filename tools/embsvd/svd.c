/* svd.c -- the CMSIS-SVD reader that embsvd and embsim share (svd.h):
 * a small XML reader, derivedFrom resolved on the XML, the device read
 * from it, every struct laid out as svdconv lays it out, and the
 * registers flattened.
 *
 * ISO C and standalone, like embar: no part of the compiler is needed. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svd.h"

const char *svd_file = "?";
const char *svd_prog = "embsvd";
int svd_status = 1, svd_quiet;

void svd_die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: %s: ", svd_prog, svd_file);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(svd_status);
}

void *svd_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "%s: out of memory\n", svd_prog);
        exit(svd_status);
    }
    return p;
}

void *svd_grow(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "%s: out of memory\n", svd_prog);
        exit(svd_status);
    }
    return p;
}

char *svd_dup(const char *s, size_t n)
{
    char *r = svd_alloc(n + 1);
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
    const struct xnode *from;   /* derivedFrom: the element this was made from */
    int resolving;
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
    *buf = svd_grow(*buf, *len + n + 1);
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
            if (!e) svd_die("line %d: an unclosed <?", x->line);
            xp_adv(x, (size_t)(e - x->p) + 2);
        } else if (!strncmp(x->p, "<!--", 4)) {
            const char *e = strstr(x->p, "-->");
            if (!e) svd_die("line %d: an unclosed comment", x->line);
            xp_adv(x, (size_t)(e - x->p) + 3);
        } else if (!strncmp(x->p, "<!DOCTYPE", 9)) {
            const char *e = strchr(x->p, '>');
            if (!e) svd_die("line %d: an unclosed DOCTYPE", x->line);
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
        svd_die("line %d: a name was expected, found '%.10s'", x->line, a);
    return svd_dup(a, (size_t)(x->p - a));
}

static struct xnode *xp_element(struct xp *x)
{
    if (*x->p != '<')
        svd_die("line %d: an element was expected", x->line);
    struct xnode *n = svd_alloc(sizeof *n);
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
            svd_die("line %d: attribute %s has no value", x->line, an);
        xp_adv(x, 1);
        xp_skip_ws(x);
        char q = *x->p;
        if (q != '"' && q != '\'')
            svd_die("line %d: attribute %s's value is not quoted", x->line, an);
        xp_adv(x, 1);
        const char *a = x->p;
        while (*x->p && *x->p != q)
            xp_adv(x, 1);
        if (!*x->p)
            svd_die("line %d: attribute %s's value is not closed", x->line, an);
        char *v = NULL;
        size_t vl = 0;
        xp_text(&v, &vl, a, (size_t)(x->p - a));
        xp_adv(x, 1);
        n->attr = svd_grow(n->attr, (size_t)(n->nattr + 1) * sizeof *n->attr);
        n->attr[n->nattr].name = an;
        n->attr[n->nattr].value = v ? v : svd_dup("", 0);
        n->nattr++;
    }
    char *text = NULL;
    size_t tl = 0;
    for (;;) {
        if (!*x->p)
            svd_die("line %d: <%s> is not closed", n->line, n->name);
        if (!strncmp(x->p, "</", 2)) {
            xp_adv(x, 2);
            char *cn = xp_name(x);
            if (strcmp(cn, n->name))
                svd_die("line %d: </%s> closes <%s> of line %d", x->line, cn,
                    n->name, n->line);
            free(cn);
            xp_skip_ws(x);
            if (*x->p != '>')
                svd_die("line %d: '>' was expected", x->line);
            xp_adv(x, 1);
            break;
        }
        if (!strncmp(x->p, "<!--", 4)) {
            const char *e = strstr(x->p, "-->");
            if (!e) svd_die("line %d: an unclosed comment", x->line);
            xp_adv(x, (size_t)(e - x->p) + 3);
        } else if (!strncmp(x->p, "<![CDATA[", 9)) {
            const char *e = strstr(x->p, "]]>");
            if (!e) svd_die("line %d: an unclosed CDATA", x->line);
            text = svd_grow(text, tl + (size_t)(e - x->p) + 1);
            memcpy(text + tl, x->p + 9, (size_t)(e - x->p) - 9);
            tl += (size_t)(e - x->p) - 9;
            text[tl] = 0;
            xp_adv(x, (size_t)(e - x->p) + 3);
        } else if (!strncmp(x->p, "<?", 2)) {
            const char *e = strstr(x->p, "?>");
            if (!e) svd_die("line %d: an unclosed <?", x->line);
            xp_adv(x, (size_t)(e - x->p) + 2);
        } else if (*x->p == '<') {
            struct xnode *k = xp_element(x);
            if (n->nkid == n->capkid) {
                n->capkid = n->capkid ? 2 * n->capkid : 4;
                n->kid = svd_grow(n->kid, (size_t)n->capkid * sizeof *n->kid);
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
        svd_die("line %d: text after the document's end", x.line);
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
unsigned long long svd_num(const char *s, const char *what, int line)
{
    if (!s)
        svd_die("line %d: %s is missing", line, what);
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
            svd_die("line %d: %s '%s' is not a number", line, what, s);
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
        svd_die("line %d: %s '%s' is not a number", line, what, s);
    return v;
}

/* ---- derivedFrom, resolved in the XML ---------------------------------
 *
 * A register, cluster, field or enumeratedValues derivedFrom another is
 * that element with what the derived one says written over it: element by
 * element, the derived one's children replace the base's of the same
 * name, and the rest are inherited. It is done on the XML, before
 * anything is read, so the reader below never sees a derivedFrom except a
 * peripheral's (which shares the base's struct, below). */

static struct xnode *g_periphs;         /* <peripherals> */

static void xadd(struct xnode *n, struct xnode *k)
{
    if (n->nkid == n->capkid) {
        n->capkid = n->capkid ? 2 * n->capkid : 4;
        n->kid = svd_grow(n->kid, (size_t)n->capkid * sizeof *n->kid);
    }
    n->kid[n->nkid++] = k;
}

/* NAME with [%s] or %s taken out: what a derivedFrom path calls it */
char *svd_plain_name(const char *s)
{
    char *o = svd_alloc(strlen(s) + 1);
    size_t n = 0;
    for (; *s; s++) {
        if (!strncmp(s, "[%s]", 4)) {
            s += 3;
            continue;
        }
        if (!strncmp(s, "%s", 2)) {
            s++;
            continue;
        }
        o[n++] = *s;
    }
    o[n] = 0;
    return o;
}

static int name_is(const struct xnode *n, const char *want, size_t wl)
{
    const char *nm = kidtext(n, "name");
    if (!nm)
        return 0;
    if (strlen(nm) == wl && !strncmp(nm, want, wl))
        return 1;
    char *p = svd_plain_name(nm);
    int r = strlen(p) == wl && !strncmp(p, want, wl);
    free(p);
    return r;
}

static int is_dim_elem(const char *e)
{
    return !strcmp(e, "dim") || !strcmp(e, "dimIncrement") ||
           !strcmp(e, "dimIndex") || !strcmp(e, "dimName") ||
           !strcmp(e, "dimArrayIndex");
}

static int is_bit_elem(const char *e)
{
    return !strcmp(e, "bitOffset") || !strcmp(e, "bitWidth") ||
           !strcmp(e, "lsb") || !strcmp(e, "msb") || !strcmp(e, "bitRange");
}

/* a field's bit position, as far as f says it: 1 the lsb, 2 the width */
static int field_bits(const struct xnode *f, long *lsb, long *width)
{
    int got = 0;
    unsigned msb, l;
    if (kidtext(f, "bitRange")) {
        if (sscanf(kidtext(f, "bitRange"), " [%u:%u]", &msb, &l) != 2)
            svd_die("line %d: bitRange '%s'", f->line, kidtext(f, "bitRange"));
        *lsb = (long)l;
        *width = (long)msb - (long)l + 1;
        return 3;
    }
    if (kidtext(f, "lsb") && kidtext(f, "msb")) {
        *lsb = (long)svd_num(kidtext(f, "lsb"), "lsb", f->line);
        *width = (long)svd_num(kidtext(f, "msb"), "msb", f->line) - *lsb + 1;
        return 3;
    }
    if (kidtext(f, "bitOffset")) {
        *lsb = (long)svd_num(kidtext(f, "bitOffset"), "bitOffset", f->line);
        got |= 1;
    }
    if (kidtext(f, "bitWidth")) {
        *width = (long)svd_num(kidtext(f, "bitWidth"), "bitWidth", f->line);
        got |= 2;
    }
    return got;
}

static struct xnode *xleaf(const char *name, long v, int line)
{
    struct xnode *n = svd_alloc(sizeof *n);
    char b[24];
    n->name = (char *)name;
    snprintf(b, sizeof b, "%ld", v);
    n->text = svd_dup(b, strlen(b));
    n->line = line;
    return n;
}

static struct xnode *merge(const struct xnode *base, const struct xnode *d)
{
    struct xnode *n = svd_alloc(sizeof *n);
    n->name = d->name;
    n->text = d->text;
    n->line = d->line;
    n->from = base;
    for (int i = 0; i < d->nattr; i++)
        if (strcmp(d->attr[i].name, "derivedFrom")) {
            n->attr = svd_grow(n->attr, (size_t)(n->nattr + 1) * sizeof *n->attr);
            n->attr[n->nattr++] = d->attr[i];
        }
    /* an array's dim comes with it only if the new name still has a %s
     * for it: TimerCtrl1 derivedFrom an array is one register */
    const char *dn = kidtext(d, "name");
    int keepdim = dn && strstr(dn, "%s");
    /* a field's position is one thing written three ways: a derived
     * field that moves it keeps the base's width unless it says one */
    long lsb = 0, width = 1, dl = 0, dw = 1;
    int bits = !strcmp(d->name, "field") ? field_bits(d, &dl, &dw) : 0;
    if (bits) {
        field_bits(base, &lsb, &width);
        xadd(n, xleaf("bitOffset", bits & 1 ? dl : lsb, d->line));
        xadd(n, xleaf("bitWidth", bits & 2 ? dw : width, d->line));
    }
    for (int i = 0; i < base->nkid; i++) {
        struct xnode *k = base->kid[i];
        if (!keepdim && is_dim_elem(k->name))
            continue;
        if (bits && is_bit_elem(k->name))
            continue;
        if (strcmp(k->name, "enumeratedValue") && kid(d, k->name))
            continue;
        xadd(n, k);
    }
    for (int i = 0; i < d->nkid; i++)
        if (!bits || !is_bit_elem(d->kid[i]->name))
            xadd(n, d->kid[i]);
    return n;
}

static struct xnode *find_periph(const char *s, size_t l)
{
    for (int i = 0; i < g_periphs->nkid; i++)
        if (!strcmp(g_periphs->kid[i]->name, "peripheral") &&
            name_is(g_periphs->kid[i], s, l))
            return g_periphs->kid[i];
    return NULL;
}

static int find_kid(const struct xnode *c, const char *tag, const char *s,
                    size_t l)
{
    for (int i = 0; c && i < c->nkid; i++)
        if (!strcmp(c->kid[i]->name, tag) && name_is(c->kid[i], s, l))
            return i;
    return -1;
}

static struct xnode *resolve_at(struct xnode *c, int i, int depth);

/* the element a derivedFrom names: in the same scope by its name, or by
 * the full path PERIPHERAL.CLUSTER...NAME */
static struct xnode *lookup(struct xnode *c, const char *tag,
                            const char *path, int line, int depth)
{
    const char *dot = strchr(path, '.');
    if (!dot) {
        int i = find_kid(c, tag, path, strlen(path));
        if (i < 0)
            svd_die("line %d: derivedFrom=\"%s\": there is no %s %s beside it "
                "(another scope is named PERIPHERAL.%s)", line, path, tag,
                path, path);
        return resolve_at(c, i, depth + 1);
    }
    struct xnode *p = find_periph(path, (size_t)(dot - path));
    if (!p)
        svd_die("line %d: derivedFrom=\"%s\": there is no peripheral %.*s", line,
            path, (int)(dot - path), path);
    for (int hops = 0; !kid(p, "registers") && attr(p, "derivedFrom"); hops++) {
        const char *b = attr(p, "derivedFrom");
        if (hops > 64 || !(p = find_periph(b, strlen(b))))
            svd_die("line %d: derivedFrom=\"%s\": the peripheral's registers "
                "cannot be found", line, path);
    }
    c = kid(p, "registers");
    for (const char *s = dot + 1;; s = dot + 1) {
        dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        int i;
        if (!dot) {
            i = find_kid(c, tag, s, l);
            if (i < 0)
                svd_die("line %d: derivedFrom=\"%s\": there is no %s %.*s there",
                    line, path, tag, (int)l, s);
            return resolve_at(c, i, depth + 1);
        }
        if ((i = find_kid(c, "cluster", s, l)) >= 0) {
            c = resolve_at(c, i, depth + 1);
        } else if (!strcmp(tag, "field") &&
                   (i = find_kid(c, "register", s, l)) >= 0) {
            c = kid(resolve_at(c, i, depth + 1), "fields");
        } else {
            svd_die("line %d: derivedFrom=\"%s\": there is no cluster %.*s there",
                line, path, (int)l, s);
        }
    }
}

/* c->kid[i], with its derivedFrom resolved and written back */
static struct xnode *resolve_at(struct xnode *c, int i, int depth)
{
    struct xnode *n = c->kid[i];
    const char *df = attr(n, "derivedFrom");
    if (!df)
        return n;
    const char *nm = kidtext(n, "name");
    if (n->resolving || depth > 64)
        svd_die("line %d: %s %s: derivedFrom goes round in a circle", n->line,
            n->name, nm ? nm : "?");
    if (!strcmp(n->name, "cluster") && (kid(n, "register") || kid(n, "cluster")))
        svd_die("line %d: cluster %s is derivedFrom %s and has registers of its "
            "own; merging the two is not supported yet", n->line,
            nm ? nm : "?", df);
    n->resolving = 1;
    struct xnode *b = lookup(c, n->name, df, n->line, depth);
    n->resolving = 0;
    struct xnode *m = merge(b, n);
    c->kid[i] = m;
    return m;
}

/* an enumeratedValues derivedFrom another is found by its name (the last
 * part of a path), in the same peripheral first, then anywhere */
static struct xnode *find_enums(struct xnode *n, const char *name, size_t l,
                                const struct xnode *self)
{
    if (!strcmp(n->name, "enumeratedValues") && n != self && name_is(n, name, l))
        return n;
    for (int i = 0; i < n->nkid; i++) {
        struct xnode *r = find_enums(n->kid[i], name, l, self);
        if (r)
            return r;
    }
    return NULL;
}

static void resolve_enums(struct xnode *f, struct xnode *periph, int depth)
{
    for (int i = 0; i < f->nkid; i++) {
        struct xnode *e = f->kid[i];
        const char *df = attr(e, "derivedFrom");
        if (strcmp(e->name, "enumeratedValues") || !df)
            continue;
        if (depth > 64)
            svd_die("line %d: enumeratedValues derivedFrom goes round in a circle",
                e->line);
        const char *last = strrchr(df, '.');
        last = last ? last + 1 : df;
        struct xnode *b = find_enums(periph, last, strlen(last), e);
        if (!b)
            b = find_enums(g_periphs, last, strlen(last), e);
        if (!b)
            svd_die("line %d: enumeratedValues derivedFrom=\"%s\": there are none "
                "of that name", e->line, df);
        if (attr(b, "derivedFrom")) {
            struct xnode tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.name = "field";
            tmp.kid = &b;
            tmp.nkid = tmp.capkid = 1;
            resolve_enums(&tmp, periph, depth + 1);
            b = tmp.kid[0];
        }
        f->kid[i] = merge(b, e);
    }
}

static void resolve_walk(struct xnode *c, struct xnode *periph, int depth)
{
    if (depth > 32)
        svd_die("line %d: clusters nested more than 32 deep", c->line);
    for (int i = 0; i < c->nkid; i++) {
        const char *t = c->kid[i]->name;
        if (strcmp(t, "register") && strcmp(t, "cluster"))
            continue;
        struct xnode *k = resolve_at(c, i, 0);
        if (!strcmp(t, "cluster")) {
            resolve_walk(k, periph, depth + 1);
            continue;
        }
        struct xnode *fs = kid(k, "fields");
        for (int j = 0; fs && j < fs->nkid; j++)
            if (!strcmp(fs->kid[j]->name, "field"))
                resolve_enums(resolve_at(fs, j, 0), periph, 0);
    }
}

/* ---- the device ------------------------------------------------------ */


struct svd_node **svd_types;   /* every cluster struct, to share and to name */
int svd_ntypes;

static const char *const access_names[] = {
    "read-write", "read-only", "write-only", "writeOnce", "read-writeOnce",
};

/* an SVD access type, in its schema spelling (Nordic writes
 * read-writeonce), or die */
static const char *access_canon(const char *a, int line)
{
    for (size_t i = 0; i < sizeof access_names / sizeof access_names[0]; i++) {
        const char *s = access_names[i], *t = a;
        while (*s && tolower((unsigned char)*s) == tolower((unsigned char)*t))
            s++, t++;
        if (!*s && !*t)
            return access_names[i];
    }
    svd_die("line %d: access '%s' is not an SVD access type", line, a);
    return NULL;
}

/* 0 read-write, 1 read-only, 2 write-only (and writeOnce) */
static int access_of(const char *canon)
{
    return !strcmp(canon, "read-only") ? 1
         : !strcmp(canon, "write-only") || !strcmp(canon, "writeOnce") ? 2 : 0;
}

static int svd_bool(const char *s)
{
    return s && (!strcmp(s, "true") || !strcmp(s, "1"));
}

static struct svd_props props_in(const struct xnode *n, struct svd_props p)
{
    if (kidtext(n, "size"))
        p.size = (int)svd_num(kidtext(n, "size"), "size", n->line);
    if (kidtext(n, "access")) {
        p.acc = access_canon(kidtext(n, "access"), n->line);
    }
    if (kidtext(n, "resetValue"))
        p.reset = svd_num(kidtext(n, "resetValue"), "resetValue", n->line);
    if (kidtext(n, "resetMask")) {
        p.rmask = svd_num(kidtext(n, "resetMask"), "resetMask", n->line);
        p.rmask_set = 1;
    }
    return p;
}

/* NAME%s with "with" for the %s; strip makes NAME[%s] NAMEwith */
char *svd_subst(const char *name, const char *with, int strip)
{
    const char *pct = strstr(name, "%s");
    size_t pre = (size_t)(pct - name), post = 2;
    if (strip && pct > name && pct[-1] == '[' && pct[2] == ']') {
        pre--;
        post++;
    }
    char *o = svd_alloc(strlen(name) + strlen(with) + 1);
    memcpy(o, name, pre);
    strcpy(o + pre, with);
    strcat(o, pct + post);
    return o;
}

/* the names dimIndex gives the elements: A,B,C or 0-7 or A-D, else 0..n-1 */
static char **dim_names(const char *t, int n, int line)
{
    char **v = svd_alloc((size_t)n * sizeof *v);
    if (!t) {
        for (int k = 0; k < n; k++) {
            char b[24];
            snprintf(b, sizeof b, "%d", k);
            v[k] = svd_dup(b, strlen(b));
        }
        return v;
    }
    int lo, hi;
    char c1, c2;
    if (strchr(t, ',')) {
        const char *q = t;
        for (int k = 0; k < n; k++) {
            if (!q)
                svd_die("line %d: dimIndex '%s' names fewer than %d", line, t, n);
            while (isspace((unsigned char)*q))
                q++;
            size_t l = strcspn(q, ",");
            while (l && isspace((unsigned char)q[l - 1]))
                l--;
            v[k] = svd_dup(q, l);
            q = strchr(q, ',');
            if (q)
                q++;
        }
        if (q)
            svd_die("line %d: dimIndex '%s' names more than %d", line, t, n);
    } else if (sscanf(t, "%d-%d", &lo, &hi) == 2) {
        if (hi - lo + 1 != n)
            svd_die("line %d: dimIndex '%s' names %d, dim is %d", line, t,
                hi - lo + 1, n);
        for (int k = 0; k < n; k++) {
            char b[24];
            snprintf(b, sizeof b, "%d", lo + k);
            v[k] = svd_dup(b, strlen(b));
        }
    } else if (sscanf(t, " %c-%c", &c1, &c2) == 2 &&
               isalpha((unsigned char)c1) && isalpha((unsigned char)c2)) {
        if (c2 - c1 + 1 != n)
            svd_die("line %d: dimIndex '%s' names %d, dim is %d", line, t,
                c2 - c1 + 1, n);
        for (int k = 0; k < n; k++) {
            char b[2] = { (char)(c1 + k), 0 };
            v[k] = svd_dup(b, 1);
        }
    } else if (n == 1 && !strchr(t, '-')) {
        v[0] = svd_dup(t, strlen(t));
    } else {
        svd_die("line %d: dimIndex '%s' is not a list, nor a range like 0-7 or "
            "A-D", line, t);
    }
    return v;
}

/* dim, dimIncrement and dimIndex of a register, cluster or peripheral */
static void read_dim(const struct xnode *x, const char *what, const char *name,
                     int *dim, unsigned long long *inc, int *bracket,
                     char ***idx)
{
    const char *d = kidtext(x, "dim");
    const char *pct = strstr(name, "%s");
    *dim = 0;
    *bracket = 0;
    if (!d) {
        if (pct)
            svd_die("line %d: %s %s has a %%s in its name but no <dim>", x->line,
                what, name);
        return;
    }
    unsigned long long n = svd_num(d, "dim", x->line);
    if (n < 1 || n > 65536)
        svd_die("line %d: %s %s: dim %llu", x->line, what, name, n);
    *dim = (int)n;
    *inc = svd_num(kidtext(x, "dimIncrement"), "dimIncrement", x->line);
    if (!pct)
        svd_die("line %d: %s array %s has no %%s in its name", x->line, what, name);
    if (strstr(pct + 2, "%s"))
        svd_die("line %d: %s %s has two %%s in its name", x->line, what, name);
    *bracket = pct > name && pct[-1] == '[' && pct[2] == ']';
    if (*bracket && pct[3])
        svd_die("line %d: %s %s: [%%s] is only understood at the end of a name",
            x->line, what, name);
    *idx = dim_names(kidtext(x, "dimIndex"), *dim, x->line);
}

/* an enumeratedValue's value: a number, or #binary with x for "any" */
static void enum_value(struct svd_enumval *e, const char *s, int line)
{
    while (isspace((unsigned char)*s) || *s == '+')
        s++;
    int bin = *s == '#' || (s[0] == '0' && (s[1] == 'b' || s[1] == 'B'));
    if (!bin) {
        e->value = svd_num(s, "enumeratedValue value", line);
        e->care = ~0ULL;
        return;
    }
    s += *s == '#' ? 1 : 2;
    e->value = 0;
    e->care = 0;
    int n = 0;
    for (; *s && !isspace((unsigned char)*s); s++, n++) {
        e->value <<= 1;
        e->care <<= 1;
        if (*s == '1' || *s == '0') {
            e->value |= (unsigned)(*s - '0');
            e->care |= 1;
        } else if (*s != 'x' && *s != 'X') {
            svd_die("line %d: enumeratedValue value: '%c' is not 0, 1 or x", line,
                *s);
        }
    }
    if (!n)
        svd_die("line %d: an enumeratedValue value with no digits", line);
    if (n < 64)
        e->care |= ~0ULL << n;
}

static void add_field(struct svd_node *r, struct svd_field *fd, int line)
{
    if (fd->width < 1 || fd->lsb < 0 || fd->lsb + fd->width > r->size)
        svd_die("line %d: field %s.%s, bits %d..%d, is outside the %d-bit "
            "register", line, r->name, fd->name, fd->lsb,
            fd->lsb + fd->width - 1, r->size);
    r->f = svd_grow(r->f, (size_t)(r->nf + 1) * sizeof *r->f);
    r->f[r->nf++] = *fd;
}

/* The fields of <register>, by bitOffset/bitWidth, lsb/msb or
 * bitRange [msb:lsb]; a dim array of them is expanded. */
static void read_fields(struct svd_node *r, const struct xnode *rn)
{
    const struct xnode *fs = kid(rn, "fields");
    for (int i = 0; fs && i < fs->nkid; i++) {
        const struct xnode *f = fs->kid[i];
        if (strcmp(f->name, "field"))
            continue;
        struct svd_field fd;
        memset(&fd, 0, sizeof fd);
        fd.name = kidtext(f, "name");
        fd.desc = kidtext(f, "description");
        if (!fd.name)
            svd_die("line %d: a field without a name", f->line);
        fd.acc = kidtext(f, "access") ? access_canon(kidtext(f, "access"),
                                                     f->line) : r->acc;
        if (kidtext(f, "bitOffset")) {
            fd.lsb = (int)svd_num(kidtext(f, "bitOffset"), "bitOffset", f->line);
            fd.width = kidtext(f, "bitWidth")
                ? (int)svd_num(kidtext(f, "bitWidth"), "bitWidth", f->line) : 1;
        } else if (kidtext(f, "lsb")) {
            fd.lsb = (int)svd_num(kidtext(f, "lsb"), "lsb", f->line);
            fd.width = (int)svd_num(kidtext(f, "msb"), "msb", f->line) - fd.lsb + 1;
        } else if (kidtext(f, "bitRange")) {
            unsigned msb, lsb;
            if (sscanf(kidtext(f, "bitRange"), " [%u:%u]", &msb, &lsb) != 2)
                svd_die("line %d: bitRange '%s'", f->line, kidtext(f, "bitRange"));
            fd.lsb = (int)lsb;
            fd.width = (int)msb - (int)lsb + 1;
        } else {
            svd_die("line %d: field %s has no bit position", f->line, fd.name);
        }
        for (int k = 0; k < f->nkid; k++) {
            const struct xnode *ev = f->kid[k];
            if (strcmp(ev->name, "enumeratedValues"))
                continue;
            const char *usage = kidtext(ev, "usage");
            for (int m = 0; m < ev->nkid; m++) {
                const struct xnode *v = ev->kid[m];
                if (strcmp(v->name, "enumeratedValue"))
                    continue;
                struct svd_enumval e;
                memset(&e, 0, sizeof e);
                e.name = kidtext(v, "name");
                e.desc = kidtext(v, "description");
                e.usage = usage ? usage : "read-write";
                e.isdef = svd_bool(kidtext(v, "isDefault"));
                if (!e.name)
                    svd_die("line %d: an enumeratedValue without a name", v->line);
                if (kidtext(v, "value"))
                    enum_value(&e, kidtext(v, "value"), v->line);
                else if (!e.isdef)
                    svd_die("line %d: enumeratedValue %s has no value", v->line,
                        e.name);
                fd.ev = svd_grow(fd.ev, (size_t)(fd.nev + 1) * sizeof *fd.ev);
                fd.ev[fd.nev++] = e;
            }
        }
        int dim, bracket;
        unsigned long long inc = 0;
        char **idx = NULL;
        read_dim(f, "field", fd.name, &dim, &inc, &bracket, &idx);
        if (!dim) {
            add_field(r, &fd, f->line);
            continue;
        }
        for (int k = 0; k < dim; k++) {
            struct svd_field c = fd;
            c.name = svd_subst(fd.name, idx[k], 1);
            c.lsb = fd.lsb + k * (int)inc;
            add_field(r, &c, f->line);
        }
    }
}

static void read_nodes(struct svd_node **kids, int *nkid, const struct xnode *c,
                       struct svd_props pr, const char *tbase, int depth);

static void read_register(struct svd_node *r, const struct xnode *x,
                          struct svd_props pr)
{
    r->line = x->line;
    r->name = kidtext(x, "name");
    if (!r->name)
        svd_die("line %d: a register without a name", x->line);
    r->desc = kidtext(x, "description");
    r->off = svd_num(kidtext(x, "addressOffset"), "addressOffset", x->line);
    pr = props_in(x, pr);
    r->size = pr.size;
    if (r->size != 8 && r->size != 16 && r->size != 32 && r->size != 64)
        svd_die("line %d: register %s is %d bits wide", x->line, r->name, r->size);
    r->acc = pr.acc ? pr.acc : "read-write";
    r->access = access_of(r->acc);
    r->reset = pr.reset;
    r->rmask = pr.rmask_set ? pr.rmask
             : r->size == 64 ? ~0ULL : (1ULL << r->size) - 1;
    r->alt = kidtext(x, "alternateRegister");
    if (!r->alt)
        r->alt = kidtext(x, "alternateGroup");
    read_dim(x, "register", r->name, &r->dim, &r->inc, &r->bracket, &r->idx);
    read_fields(r, x);
}

/* what a derived element's registers were copied from */
static const struct xnode *content_key(const struct xnode *x)
{
    while (x->from)
        x = x->from;
    return x;
}

static int props_eq(const struct svd_props *a, const struct svd_props *b)
{
    return a->size == b->size && a->reset == b->reset &&
           a->rmask_set == b->rmask_set && a->rmask == b->rmask &&
           (a->acc == b->acc || (a->acc && b->acc && !strcmp(a->acc, b->acc)));
}

static void read_cluster(struct svd_node *c, const struct xnode *x,
                         struct svd_props pr, const char *pbase, int depth)
{
    c->cluster = 1;
    c->line = x->line;
    c->name = kidtext(x, "name");
    if (!c->name)
        svd_die("line %d: a cluster without a name", x->line);
    c->desc = kidtext(x, "description");
    c->off = svd_num(kidtext(x, "addressOffset"), "addressOffset", x->line);
    c->alt = kidtext(x, "alternateCluster");
    read_dim(x, "cluster", c->name, &c->dim, &c->inc, &c->bracket, &c->idx);
    pr = props_in(x, pr);
    /* its struct: headerStructName, else dimName, else the enclosing
     * struct's name and its own, as svdconv names it */
    const char *h = kidtext(x, "headerStructName");
    if (!h)
        h = kidtext(x, "dimName");
    char *pn = svd_plain_name(c->name);
    if (h) {
        c->tname = svd_dup(h, strlen(h));
    } else {
        size_t tl = strlen(pbase) + strlen(pn) + 2;
        c->tname = svd_alloc(tl);
        snprintf(c->tname, tl, "%s_%s", pbase, pn);
    }
    free(pn);
    for (char *q = c->tname; *q; q++)
        if (!isalnum((unsigned char)*q))
            *q = '_';
    /* a cluster derivedFrom another, with the same registers read the same
     * way, is the same struct */
    c->key = content_key(x);
    c->pr = pr;
    for (int i = 0; i < svd_ntypes; i++)
        if (svd_types[i]->key == c->key && props_eq(&svd_types[i]->pr, &pr)) {
            c->type = svd_types[i];
            return;
        }
    for (int i = 0; i < svd_ntypes; i++)
        if (!strcmp(svd_types[i]->tname, c->tname))
            svd_die("line %d: cluster %s and the one at line %d would both be "
                "struct %s_Type, with different registers", x->line, c->name,
                svd_types[i]->line, c->tname);
    c->type = c;
    svd_types = svd_grow(svd_types, (size_t)(svd_ntypes + 1) * sizeof *svd_types);
    svd_types[svd_ntypes++] = c;
    read_nodes(&c->kid, &c->nkid, x, pr, c->tname, depth + 1);
}

static void read_nodes(struct svd_node **kids, int *nkid, const struct xnode *c,
                       struct svd_props pr, const char *tbase, int depth)
{
    int n = 0;
    for (int i = 0; i < c->nkid; i++)
        if (!strcmp(c->kid[i]->name, "register") ||
            !strcmp(c->kid[i]->name, "cluster"))
            n++;
    *kids = svd_alloc((size_t)n * sizeof **kids);
    *nkid = 0;
    for (int i = 0; i < c->nkid; i++) {
        const struct xnode *k = c->kid[i];
        if (!strcmp(k->name, "register"))
            read_register(&(*kids)[(*nkid)++], k, pr);
        else if (!strcmp(k->name, "cluster"))
            read_cluster(&(*kids)[(*nkid)++], k, pr, tbase, depth);
    }
}

static void add_irq(struct svd_device *d, struct svd_periph *p, const struct xnode *in)
{
    struct svd_irq q;
    q.name = kidtext(in, "name");
    q.desc = kidtext(in, "description");
    if (!q.name)
        svd_die("line %d: an interrupt without a name", in->line);
    q.value = (int)svd_num(kidtext(in, "value"), "value", in->line);
    p->irq = svd_grow(p->irq, (size_t)(p->nirq + 1) * sizeof *p->irq);
    p->irq[p->nirq++] = q;
    for (int j = 0; j < d->nirq; j++)
        if (!strcmp(d->irq[j].name, q.name)) {
            if (d->irq[j].value != q.value)
                svd_die("line %d: interrupt %s is %d here and %d before", in->line,
                    q.name, q.value, d->irq[j].value);
            return;
        }
    d->irq = svd_grow(d->irq, (size_t)(d->nirq + 1) * sizeof *d->irq);
    d->irq[d->nirq++] = q;
}

static struct svd_device *read_device(struct xnode *root)
{
    if (strcmp(root->name, "device"))
        svd_die("the document is <%s>, not an SVD <device>", root->name);
    struct svd_device *d = svd_alloc(sizeof *d);
    d->name = kidtext(root, "name");
    d->vendor = kidtext(root, "vendor");
    d->version = kidtext(root, "version");
    d->desc = kidtext(root, "description");
    d->prefix = kidtext(root, "headerDefinitionsPrefix");
    if (!d->name)
        svd_die("the device has no name");
    d->aub = kidtext(root, "addressUnitBits")
        ? (int)svd_num(kidtext(root, "addressUnitBits"), "addressUnitBits",
                       root->line) : 8;
    if (d->aub != 8)
        svd_die("addressUnitBits is %d: offsets in units of other than 8 bits "
            "are not supported", d->aub);
    d->width = kidtext(root, "width")
        ? (int)svd_num(kidtext(root, "width"), "width", root->line) : 32;
    const struct xnode *cpu = kid(root, "cpu");
    if (cpu) {
        d->has_cpu = 1;
        d->cpu = kidtext(cpu, "name");
        d->cpurev = kidtext(cpu, "revision");
        d->endian = kidtext(cpu, "endian");
        d->prio_bits = kidtext(cpu, "nvicPrioBits")
            ? (int)svd_num(kidtext(cpu, "nvicPrioBits"), "nvicPrioBits",
                           cpu->line) : 0;
        d->fpu = svd_bool(kidtext(cpu, "fpuPresent"));
        d->fpu_dp = svd_bool(kidtext(cpu, "fpuDP"));
        d->mpu = svd_bool(kidtext(cpu, "mpuPresent"));
        d->vendor_systick = svd_bool(kidtext(cpu, "vendorSystickConfig"));
        d->num_irq = kidtext(cpu, "deviceNumInterrupts")
            ? (int)svd_num(kidtext(cpu, "deviceNumInterrupts"),
                           "deviceNumInterrupts", cpu->line) : -1;
    }
    struct svd_props dpr;
    memset(&dpr, 0, sizeof dpr);
    dpr.size = 32;
    dpr = props_in(root, dpr);

    struct xnode *ps = kid(root, "peripherals");
    if (!ps)
        svd_die("the device has no <peripherals>");
    g_periphs = ps;
    for (int i = 0; i < ps->nkid; i++)
        if (!strcmp(ps->kid[i]->name, "peripheral") && kid(ps->kid[i], "registers"))
            resolve_walk(kid(ps->kid[i], "registers"), ps->kid[i], 0);

    int cap = 0;
    for (int i = 0; i < ps->nkid; i++)
        if (!strcmp(ps->kid[i]->name, "peripheral"))
            cap += kidtext(ps->kid[i], "dim")
                ? (int)svd_num(kidtext(ps->kid[i], "dim"), "dim",
                               ps->kid[i]->line) : 1;
    d->p = svd_alloc((size_t)(cap + 1) * sizeof *d->p);
    for (int i = 0; i < ps->nkid; i++) {
        struct xnode *pn = ps->kid[i];
        if (strcmp(pn->name, "peripheral"))
            continue;
        struct svd_periph *p = &d->p[d->np++];
        p->node = pn;
        p->name = kidtext(pn, "name");
        if (!p->name)
            svd_die("line %d: a peripheral without a name", pn->line);
        p->desc = kidtext(pn, "description");
        p->group = kidtext(pn, "groupName");
        p->derived = attr(pn, "derivedFrom");
        p->base = svd_num(kidtext(pn, "baseAddress"), "baseAddress", pn->line);
        const char *h = kidtext(pn, "headerStructName");
        char *t = svd_plain_name(h ? h : p->name);
        for (char *q = t; *q; q++)
            if (!isalnum((unsigned char)*q))
                *q = '_';
        p->tname = t;
        for (int k = 0; k < pn->nkid; k++) {
            const struct xnode *a = pn->kid[k];
            if (!strcmp(a->name, "interrupt"))
                add_irq(d, p, a);
            if (!strcmp(a->name, "addressBlock")) {
                struct svd_ablock b;
                b.off = svd_num(kidtext(a, "offset"), "offset", a->line);
                b.size = svd_num(kidtext(a, "size"), "size", a->line);
                b.usage = kidtext(a, "usage");
                p->ab = svd_grow(p->ab, (size_t)(p->nab + 1) * sizeof *p->ab);
                p->ab[p->nab++] = b;
            }
        }
        const struct xnode *rs = kid(pn, "registers");
        if (rs)
            read_nodes(&p->kid, &p->nkid, rs, props_in(pn, dpr), p->tname, 0);
        /* a peripheral array, TIMER%s: one peripheral per element, all
         * with the first one's registers */
        int dim, bracket;
        unsigned long long inc = 0;
        char **idx = NULL;
        read_dim(pn, "peripheral", p->name, &dim, &inc, &bracket, &idx);
        if (dim && bracket)
            svd_die("line %d: peripheral %s: an array of peripherals written "
                "[%%s] is not supported yet (NAME%%s is)", pn->line, p->name);
        if (dim) {
            struct svd_periph first = *p;
            for (int k = 0; k < dim; k++) {
                struct svd_periph *e = &d->p[d->np - 1 + k];
                *e = first;
                e->name = svd_subst(first.name, idx[k], 0);
                e->base = first.base + (unsigned long long)k * inc;
                if (k) {
                    e->derived = d->p[d->np - 1].name;
                    e->kid = NULL;
                    e->nkid = 0;
                    e->irq = NULL;
                    e->nirq = 0;
                }
            }
            d->np += dim - 1;
        }
    }
    /* derivedFrom: the registers, and what else was not said, are the
     * base's; a chain is followed to its end */
    for (int i = 0; i < d->np; i++) {
        struct svd_periph *p = &d->p[i];
        struct svd_periph *b = p;
        for (int hops = 0; b->derived; hops++) {
            if (b->nkid)
                svd_die("line %d: %s is derivedFrom %s and has registers of its "
                    "own; merging the two is not supported yet",
                    b->node->line, b->name, b->derived);
            struct svd_periph *nb = NULL;
            for (int j = 0; j < d->np; j++)
                if (!strcmp(d->p[j].name, b->derived))
                    nb = &d->p[j];
            if (!nb)
                svd_die("line %d: %s is derivedFrom %s, which is not in the file",
                    b->node->line, b->name, b->derived);
            if (hops > d->np)
                svd_die("%s: derivedFrom goes round in a circle", p->name);
            b = nb;
        }
        p->layout = b;
        if (!p->desc) p->desc = p->layout->desc;
        if (!p->group) p->group = p->layout->group;
        if (!p->nab) {
            p->ab = p->layout->ab;
            p->nab = p->layout->nab;
        }
    }
    for (int i = 0; i < d->np; i++) {
        struct svd_periph *p = &d->p[i];
        if (p->layout != p)
            continue;
        if (!p->nkid && !svd_quiet)
            fprintf(stderr, "%s: %s: warning: peripheral %s has no "
                    "registers\n", svd_prog, svd_file, p->name);
        for (int j = 0; j < i; j++)
            if (d->p[j].layout == &d->p[j] && !strcmp(d->p[j].tname, p->tname))
                svd_die("line %d: peripherals %s and %s would both be struct "
                    "%s_Type, with registers of their own", p->node->line,
                    d->p[j].name, p->name, p->tname);
        /* headerDefinitionsPrefix is put on a peripheral's struct, not on
         * a cluster's: Nordic's CRACEN cluster and NRF_CRACEN_Type */
        size_t pl = d->prefix ? strlen(d->prefix) : 0;
        for (int j = 0; j < svd_ntypes; j++)
            if (!strncmp(svd_types[j]->tname, d->prefix ? d->prefix : "", pl) &&
                !strcmp(svd_types[j]->tname + pl, p->tname))
                svd_die("line %d: peripheral %s and cluster %s (line %d) would "
                    "both be struct %s_Type", p->node->line, p->name,
                    svd_types[j]->name, svd_types[j]->line, p->tname);
    }
    return d;
}

/* ---- the layout --------------------------------------------------------
 *
 * Every struct -- a peripheral's, a cluster's -- is laid out from its
 * members' offsets, as svdconv lays it out: a gap is a uint8_t RESERVED
 * array, members at one offset that are each other's alternates are a
 * union, and an array is a C array when its elements are packed:
 *
 *   NAME[%s], dimIncrement the element's size   NAME[dim]
 *   NAME[%s], a cluster whose struct is smaller  the struct is padded to
 *                                                dimIncrement: NAME[dim]
 *   NAME[%s], a register with a larger increment NAME0, NAME1, ... apart
 *   NAME%s                                       NAMEa, NAMEb, ... apart
 *
 * What C cannot place where the SVD says -- a register off its natural
 * alignment, elements closer together than their size -- is refused. */

struct mem {
    const struct svd_node *n;
    char *name;                 /* the C member's */
    unsigned long long off, bytes;
    int count;                  /* a C array's length, or 0 */
    int k;                      /* the element, of an array split apart */
    int seq;
};

static unsigned long long elem_bytes(const struct svd_node *n)
{
    return n->cluster ? n->type->tsize : (unsigned long long)n->size / 8;
}

static unsigned elem_align(const struct svd_node *n)
{
    return n->cluster ? n->type->talign : (unsigned)n->size / 8;
}

/* is this array one C array? */
static int carray(const struct svd_node *n)
{
    return n->dim && n->bracket && n->inc == elem_bytes(n);
}

static int mem_cmp(const void *a, const void *b)
{
    const struct mem *x = a, *y = b;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return x->seq - y->seq;
}

static struct mem *members(const struct svd_node *kids, int nkid, int *nm)
{
    int cap = 0;
    for (int i = 0; i < nkid; i++)
        cap += carray(&kids[i]) || !kids[i].dim ? 1 : kids[i].dim;
    struct mem *m = svd_alloc((size_t)(cap + 1) * sizeof *m);
    int n = 0;
    for (int i = 0; i < nkid; i++) {
        const struct svd_node *k = &kids[i];
        if (!k->dim || carray(k)) {
            m[n].n = k;
            m[n].name = k->dim ? svd_plain_name(k->name) : svd_dup(k->name, strlen(k->name));
            m[n].off = k->off;
            m[n].count = k->dim;
            m[n].bytes = k->dim ? k->inc * (unsigned long long)k->dim
                                : elem_bytes(k);
            m[n].k = -1;
            m[n].seq = n;
            n++;
            continue;
        }
        if (k->inc < elem_bytes(k))
            svd_die("line %d: array %s: its elements are %llu bytes but %llu "
                "apart, so they overlap", k->line, k->name, elem_bytes(k),
                k->inc);
        for (int e = 0; e < k->dim; e++) {
            m[n].n = k;
            m[n].name = svd_subst(k->name, k->idx[e], 1);
            m[n].off = k->off + (unsigned long long)e * k->inc;
            m[n].count = 0;
            m[n].bytes = elem_bytes(k);
            m[n].k = e;
            m[n].seq = n;
            n++;
        }
    }
    qsort(m, (size_t)n, sizeof *m, mem_cmp);
    *nm = n;
    return m;
}

static void free_members(struct mem *m, int n)
{
    for (int i = 0; i < n; i++)
        free(m[i].name);
    free(m);
}

/* may two members share an offset? alternates may, and so may a
 * read-only and a write-only register (svdconv makes them a union) */
static int may_share(const struct svd_node *a, const struct svd_node *b)
{
    if (a->alt || b->alt)
        return 1;
    return !a->cluster && !b->cluster &&
           ((a->access == 1 && b->access == 2) || (a->access == 2 && b->access == 1));
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
const char *svd_ident(const char *s)
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

void svd_put_desc(FILE *f, const char *s)
{
    for (; *s; s++)
        if (!(s[0] == '*' && s[1] == '/'))
            fputc(*s, f);
}

/* Lay out one struct's members: check them, and with f print them.
 * Returns where the last one ends; *align is the struct's alignment. */
unsigned long long svd_lay(const struct svd_node *kids, int nkid, FILE *f,
                              const char *what, unsigned *align, int *nres)
{
    int n;
    struct mem *m = members(kids, nkid, &n);
    unsigned long long at = 0;
    unsigned al = 1;
    for (int i = 0; i < n; ) {
        int j = i + 1;
        while (j < n && m[j].off == m[i].off)
            j++;
        if (m[i].off < at)
            svd_die("line %d: %s.%s at offset 0x%llx overlaps the register "
                "before it", m[i].n->line, what, m[i].name, m[i].off);
        for (int k = i; k < j; k++) {
            unsigned a = elem_align(m[k].n);
            if (a > al)
                al = a;
            if (m[k].off % a)
                svd_die("line %d: %s.%s at offset 0x%llx is not aligned to its "
                    "%u bytes, so a C struct cannot put it there", m[k].n->line,
                    what, m[k].name, m[k].off, a);
        }
        /* SVD says which register is another's alternate view; two at
         * one offset that do not say so are a mistake in the file */
        for (int k = i + 1; k < j; k++)
            if (!may_share(m[k].n, m[i].n))
                svd_die("line %d: %s.%s at offset 0x%llx overlaps %s, and "
                    "neither is the other's alternateRegister", m[k].n->line,
                    what, m[k].name, m[k].off, m[i].name);
        if (f && m[i].off > at)
            fprintf(f, "  uint8_t RESERVED%d[%llu];\n", (*nres)++, m[i].off - at);
        unsigned long long end = m[i].off;
        if (f && j - i > 1)
            fprintf(f, "  union {\n");
        for (int k = i; k < j; k++) {
            const struct svd_node *r = m[k].n;
            if (f) {
                if (r->cluster)
                    fprintf(f, "%s  __IO %s_Type %s", j - i > 1 ? "  " : "",
                            r->type->tname, svd_ident(m[k].name));
                else
                    fprintf(f, "%s  %s %s %s", j - i > 1 ? "  " : "",
                            qual_of(r->access), ctype_of(r->size),
                            svd_ident(m[k].name));
                if (m[k].count)
                    fprintf(f, "[%d]", m[k].count);
                fprintf(f, ";  /*!< 0x%03llx", m[k].off);
                if (r->desc) {
                    fputs(": ", f);
                    svd_put_desc(f, r->desc);
                }
                fputs(" */\n", f);
            }
            if (m[k].off + m[k].bytes > end)
                end = m[k].off + m[k].bytes;
        }
        if (f && j - i > 1)
            fprintf(f, "  };\n");
        at = end;
        i = j;
    }
    free_members(m, n);
    *align = al;
    return at;
}

/* Size every cluster struct under kids, innermost first, padding one to
 * the increment of the NAME[%s] array it makes. */
static void size_types(const struct svd_node *kids, int nkid)
{
    for (int i = 0; i < nkid; i++) {
        const struct svd_node *k = &kids[i];
        if (!k->cluster)
            continue;
        struct svd_node *t = k->type;
        if (!t->sized) {
            t->sized = 1;
            size_types(t->kid, t->nkid);
            t->tend = svd_lay(t->kid, t->nkid, NULL, t->tname, &t->talign, NULL);
            t->tsize = (t->tend + t->talign - 1) / t->talign * t->talign;
            if (!t->tsize)
                svd_die("line %d: cluster %s has no registers", t->line, t->name);
        }
        if (k->dim && k->bracket && k->inc > t->tsize && !t->locked &&
            k->inc % t->talign == 0)
            t->tsize = k->inc;
        t->locked = 1;
    }
}

/* ---- the registers, flattened -------------------------------------------
 *
 * Every register of a peripheral, each element of every array, with where
 * it is (from the peripheral's base) and how C reaches it. --json and
 * --show print these. */


static void flatten(const struct svd_node *kids, int nkid, const char *pre,
                    int *idx, int nidx, unsigned long long at, struct svd_flats *o)
{
    for (int i = 0; i < nkid; i++) {
        const struct svd_node *k = &kids[i];
        int cnt = k->dim ? k->dim : 1;
        for (int e = 0; e < cnt; e++) {
            unsigned long long off = at + k->off +
                (k->dim ? (unsigned long long)e * k->inc : 0);
            /* the C member: NAME, NAME[e], or the element's own name */
            char *seg = !k->dim ? svd_dup(k->name, strlen(k->name))
                      : carray(k) ? svd_plain_name(k->name)
                      : svd_subst(k->name, k->idx[e], 1);
            size_t pl = (pre ? strlen(pre) : 0) + strlen(seg) + 16;
            char *path = svd_alloc(pl);
            snprintf(path, pl, "%s%s%s", pre ? pre : "", pre ? "." : "",
                     svd_ident(seg));
            if (carray(k))
                snprintf(path + strlen(path), pl - strlen(path), "[%d]", e);
            free(seg);
            if (k->dim) {
                if (nidx >= 40)
                    svd_die("line %d: arrays nested too deep", k->line);
                idx[nidx] = e;
            }
            int ni = nidx + (k->dim ? 1 : 0);
            if (k->cluster) {
                flatten(k->type->kid, k->type->nkid, path, idx, ni, off, o);
                free(path);
                continue;
            }
            if (o->n == o->cap) {
                o->cap = o->cap ? 2 * o->cap : 64;
                o->v = svd_grow(o->v, (size_t)o->cap * sizeof *o->v);
            }
            struct svd_flat *fl = &o->v[o->n++];
            fl->r = k;
            fl->name = k->dim ? svd_subst(k->name, k->idx[e], 0)
                              : svd_dup(k->name, strlen(k->name));
            fl->path = path;
            memcpy(fl->idx, idx, (size_t)ni * sizeof *idx);
            fl->nidx = ni;
            fl->off = off;
        }
    }
}

static int flat_cmp(const void *a, const void *b)
{
    const struct svd_flat *x = a, *y = b;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return x->r->line - y->r->line;
}

struct svd_flats svd_flat_regs(const struct svd_periph *p, int sorted)
{
    struct svd_flats o;
    int idx[40];
    memset(&o, 0, sizeof o);
    flatten(p->layout->kid, p->layout->nkid, NULL, idx, 0, 0, &o);
    if (sorted)
        qsort(o.v, (size_t)o.n, sizeof *o.v, flat_cmp);
    return o;
}

void svd_free_flats(struct svd_flats *o)
{
    for (int i = 0; i < o->n; i++) {
        free(o->v[i].name);
        free(o->v[i].path);
    }
    free(o->v);
}

/* every struct sized and checked, before any output is written: what
 * one output refuses, they all refuse */
void svd_size_device(struct svd_device *d)
{
    for (int i = 0; i < d->np; i++)
        if (d->p[i].layout == &d->p[i]) {
            unsigned al;
            size_types(d->p[i].kid, d->p[i].nkid);
            svd_lay(d->p[i].kid, d->p[i].nkid, NULL, d->p[i].name, &al, NULL);
        }
}

char *svd_read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        svd_die("cannot open it");
    size_t cap = 1 << 20, n = 0;
    char *b = svd_alloc(cap);
    for (;;) {
        if (n + 65536 > cap)
            b = svd_grow(b, cap *= 2);
        size_t got = fread(b + n, 1, cap - n - 1, f);
        n += got;
        if (got == 0)
            break;
    }
    fclose(f);
    b[n] = 0;
    if (strlen(b) != n)
        svd_die("it has a NUL byte in it: not an SVD file");
    return b;
}

struct svd_device *svd_load(const char *path)
{
    svd_file = path;
    char *text = svd_read_all(path);
    struct svd_device *d = read_device(xml_parse(text));
    svd_size_device(d);
    return d;
}
