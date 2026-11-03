/* GNU's argp: argv parsed by option tables, and the help written from
 * them.
 *
 * The parsing is getopt_long's (a private state of it, so the program's
 * optind is untouched) over every table in the tree, the short options
 * of the first table that names a letter winning. The parsers are called
 * as glibc's manual has it: ARGP_KEY_INIT top-down, the options, the
 * operands (ARGP_KEY_ARG, else ARGP_KEY_ARGS), ARGP_KEY_NO_ARGS,
 * ARGP_KEY_END bottom-up, then SUCCESS or ERROR, then FINI.
 *
 * The help is laid out as glibc lays it out -- the columns, the sorting
 * of groups and clusters, where a doc string wraps -- because programs
 * test their --help against what glibc printed. ARGP_HELP_FMT changes
 * the columns, as there.
 */
#define _GNU_SOURCE 1
#include <argp.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "posix.h"
#include "../internal/getopt-r.h"

WEAK const char *argp_program_version;
WEAK void (*argp_program_version_hook)(FILE *, struct argp_state *);
WEAK const char *argp_program_bug_address;
WEAK error_t argp_err_exit_status = 64;   /* EX_USAGE */

static int oshort(const struct argp_option *o)
{
    return !(o->flags & OPTION_DOC) && o->key > 0 && o->key <= UCHAR_MAX &&
           isprint(o->key);
}

static int oend(const struct argp_option *o)
{
    return !o->key && !o->name && !o->doc && !o->group;
}

#define oalias(o)   ((o)->flags & OPTION_ALIAS)
#define ohidden(o)  ((o)->flags & OPTION_HIDDEN)
#define odoc(o)     ((o)->flags & OPTION_DOC)

WEAK int _option_is_short(const struct argp_option *o) { return oshort(o); }
WEAK int _option_is_end(const struct argp_option *o) { return oend(o); }

static const char *short_program_name(void)
{
    return program_invocation_short_name ? program_invocation_short_name : "";
}

/* ---- the column writer ----------------------------------------------------------------
 *
 * Text goes in a run at a time. A run is laid out when the margins change
 * or the column is asked for: a line that reaches the right margin breaks
 * at its last blank inside the margin -- but only inside the run, never
 * in text laid out before -- and goes on at the wrap margin. A line that
 * starts fresh starts at the left margin. */

struct col {
    FILE *out;
    char *run;
    size_t n, cap;
    int lm, wm, rm;
    int at;          /* the column the run starts at */
    int nolm;        /* the line is a wrap's at margin 0: no left margin */
    int oom;
};

static void col_emit(struct col *c, const char *s, size_t n)
{
    if (n)
        fwrite(s, 1, n, c->out);
}

static void col_spaces(struct col *c, int n)
{
    while (n-- > 0)
        fputc(' ', c->out);
}

static int isblank_(int ch) { return ch == ' ' || ch == '\t'; }

/* a line broken: the next part of it starts at the wrap margin */
static void col_wrap(struct col *c)
{
    fputc('\n', c->out);
    col_spaces(c, c->wm);
    c->at = c->wm;
    c->nolm = c->wm == 0;
}

static void col_layout(struct col *c)
{
    const char *t = c->run;
    size_t i = 0, n = c->n;
    while (i < n) {
        if (c->at == 0 && c->lm && !c->nolm) {
            col_spaces(c, c->lm);
            c->at = c->lm;
        }
        c->nolm = 0;
        size_t e = i;
        while (e < n && t[e] != '\n')
            e++;
        if ((size_t)c->at + (e - i) < (size_t)c->rm) {
            col_emit(c, t + i, e - i);
            if (e < n) {
                fputc('\n', c->out);
                c->at = 0;
                i = e + 1;
            } else {
                c->at += (int)(e - i);
                i = e;
            }
            continue;
        }
        /* too long: the last blank at or before the margin's column */
        size_t lim = c->at < c->rm ? i + (size_t)(c->rm - c->at) : i;
        size_t k = lim;
        int found = 0;
        if (c->at < c->rm) {
            for (;;) {
                if (k < e && isblank_(t[k])) {
                    found = 1;
                    break;
                }
                if (k == i)
                    break;
                k--;
            }
        }
        if (found) {
            size_t keep = k;
            while (keep > i && isblank_(t[keep - 1]))
                keep--;
            col_emit(c, t + i, keep - i);
            col_wrap(c);
            i = k + 1;
            continue;
        }
        /* one word past the margin: it ends the line where it ends */
        size_t q = lim;
        if (c->at < c->rm) {
            if (q < e)
                do
                    q++;
                while (q < e && !isblank_(t[q]));
        } else {
            while (q < e && !isblank_(t[q]))
                q++;
        }
        if (q == e) {
            col_emit(c, t + i, e - i);
            if (e < n)
                fputc('\n', c->out);
            i = e < n ? e + 1 : e;
            c->at = 0;
            continue;
        }
        col_emit(c, t + i, q - i);
        col_wrap(c);
        while (q < e && isblank_(t[q]))
            q++;
        i = q;
    }
    c->n = 0;
}

static void col_write(struct col *c, const char *s, size_t n)
{
    if (c->n + n > c->cap) {
        size_t cap = c->cap * 2 + n + 64;
        char *r = realloc(c->run, cap);
        if (!r) {
            c->oom = 1;
            return;
        }
        c->run = r;
        c->cap = cap;
    }
    memcpy(c->run + c->n, s, n);
    c->n += n;
}

static void col_puts(struct col *c, const char *s) { col_write(c, s, strlen(s)); }

static void col_putc(struct col *c, int ch)
{
    char b = (char)ch;
    col_write(c, &b, 1);
}

static void col_printf(struct col *c, const char *fmt, ...)
{
    char small[256];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (k < 0)
        return;
    if ((size_t)k < sizeof small) {
        col_write(c, small, (size_t)k);
        return;
    }
    char *big = malloc((size_t)k + 1);
    if (!big)
        return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)k + 1, fmt, ap);
    va_end(ap);
    col_write(c, big, (size_t)k);
    free(big);
}

static int col_point(struct col *c)
{
    if (c->n)
        col_layout(c);
    return c->at;
}

static int col_set_lm(struct col *c, int v)
{
    if (c->n)
        col_layout(c);
    int old = c->lm;
    c->lm = v;
    return old;
}

static int col_set_wm(struct col *c, int v)
{
    if (c->n)
        col_layout(c);
    int old = c->wm;
    c->wm = v;
    return old;
}

static void indent_to(struct col *c, int to)
{
    for (int k = to - col_point(c); k > 0; k--)
        col_putc(c, ' ');
}

/* a break before a word of `need` columns if it would reach the margin */
static void space_for(struct col *c, size_t need)
{
    col_putc(c, (size_t)col_point(c) + need >= (size_t)c->rm ? '\n' : ' ');
}

/* ---- the help's columns, and ARGP_HELP_FMT ---------------------------------------------- */

static struct uparams {
    int dup_args, dup_args_note;
    int short_opt_col, long_opt_col, doc_opt_col, opt_doc_col, header_col;
    int usage_indent, rmargin;
    int valid;
} up = { 0, 1, 2, 6, 2, 29, 1, 12, 79, 0 };

static const struct { const char *name; int is_bool; size_t off; } upnames[] = {
    { "dup-args", 1, offsetof(struct uparams, dup_args) },
    { "dup-args-note", 1, offsetof(struct uparams, dup_args_note) },
    { "short-opt-col", 0, offsetof(struct uparams, short_opt_col) },
    { "long-opt-col", 0, offsetof(struct uparams, long_opt_col) },
    { "doc-opt-col", 0, offsetof(struct uparams, doc_opt_col) },
    { "opt-doc-col", 0, offsetof(struct uparams, opt_doc_col) },
    { "header-col", 0, offsetof(struct uparams, header_col) },
    { "usage-indent", 0, offsetof(struct uparams, usage_indent) },
    { "rmargin", 0, offsetof(struct uparams, rmargin) },
};
#define NUPNAMES (sizeof upnames / sizeof upnames[0])

/* "name=value", "flag", "no-flag", separated by commas or blanks; kept
 * only when every column is left of rmargin */
static void read_help_fmt(const struct argp_state *state)
{
    const char *v = getenv("ARGP_HELP_FMT");
    if (!v)
        return;
    struct uparams np = up;
    while (*v) {
        while (isspace((unsigned char)*v))
            v++;
        if (!*v)
            break;
        if (!isalpha((unsigned char)*v)) {
            argp_failure(state, 0, 0, "Garbage in ARGP_HELP_FMT: %s", v);
            break;
        }
        const char *name = v, *a = v;
        while (isalnum((unsigned char)*a) || *a == '-' || *a == '_')
            a++;
        size_t nl = (size_t)(a - name);
        while (isspace((unsigned char)*a))
            a++;
        int unspec = 0, val = 0;
        if (*a == 0 || *a == ',') {
            unspec = 1;
        } else if (*a == '=') {
            a++;
            while (isspace((unsigned char)*a))
                a++;
        }
        if (unspec) {
            if (nl > 3 && !strncmp(name, "no-", 3)) {
                name += 3;
                nl -= 3;
            } else {
                val = 1;
            }
        } else if (isdigit((unsigned char)*a)) {
            val = atoi(a);
            while (isdigit((unsigned char)*a))
                a++;
            while (isspace((unsigned char)*a))
                a++;
        }
        size_t k;
        for (k = 0; k < NUPNAMES; k++)
            if (strlen(upnames[k].name) == nl && !strncmp(name, upnames[k].name, nl))
                break;
        if (k == NUPNAMES)
            argp_failure(state, 0, 0, "%.*s: Unknown ARGP_HELP_FMT parameter", (int)nl, name);
        else if (unspec && !upnames[k].is_bool)
            argp_failure(state, 0, 0, "%.*s: ARGP_HELP_FMT parameter requires a value",
                         (int)nl, name);
        else
            *(int *)((char *)&np + upnames[k].off) = val;
        v = a;
        if (*v == ',')
            v++;
    }
    for (size_t k = 0; k < NUPNAMES; k++) {
        if (upnames[k].is_bool || upnames[k].off == offsetof(struct uparams, rmargin))
            continue;
        if (*(int *)((char *)&np + upnames[k].off) >= np.rmargin) {
            argp_failure(state, 0, 0,
                         "ARGP_HELP_FMT: %s value is less than or equal to %s",
                         "rmargin", upnames[k].name);
            return;
        }
    }
    up = np;
    up.valid = 1;
}

/* ---- the parser's state, and the input each argp has ----------------------------------- */

struct group {
    argp_parser_t parser;
    const struct argp *argp;
    size_t short_end;          /* its short options end here in shorts */
    unsigned args_processed;
    struct group *parent;
    unsigned parent_index;
    void *input, **child_inputs, *hook;
};

struct parser {
    const struct argp *argp;
    char *shorts;
    struct option *longs;
    int *long_group, *long_key, nlongs;
    struct __getopt_state go;
    struct group *groups, *egroup;
    void **child_inputs;
    int try_getopt;
    struct argp_state state;
};

WEAK void *_argp_input(const struct argp *argp, const struct argp_state *state)
{
    if (state && state->pstate) {
        struct parser *p = state->pstate;
        for (struct group *g = p->groups; g < p->egroup; g++)
            if (g->argp == argp)
                return g->input;
    }
    return 0;
}

static const char *filter(const char *doc, int key, const struct argp *argp,
                          const struct argp_state *state)
{
    if (argp && argp->help_filter)
        return argp->help_filter(key, doc, _argp_input(argp, state));
    return doc;
}

static void unfilter(const char *got, const char *doc)
{
    if (got && got != doc)
        free((char *)got);
}

/* ---- the help's list of options ----------------------------------------------------------
 *
 * An entry is an option with its aliases. Entries are grouped -- an
 * option's group, else the group before it, a header starting the next
 * one -- and a child argp with a header or a group of its own puts its
 * entries in a cluster, which sorts as a whole among its parent's. */

struct cluster {
    const char *header;
    int index, group, depth;
    struct cluster *parent;
    const struct argp *argp;
    struct cluster *next;
};

struct entry {
    const struct argp_option *opt;
    unsigned num;
    const char *shorts;        /* its short options that no earlier one took */
    int group;
    struct cluster *cluster;
    const struct argp *argp;
    unsigned ord;
};

struct hol {
    struct entry *e;
    unsigned n, cap;
    char *shorts;              /* every entry's, one after another */
    size_t nshorts, shorts_cap;
    struct cluster *clusters;
    int oom;
};

static void hol_entries(struct hol *h, const struct argp *argp, struct cluster *cl)
{
    const struct argp_option *o = argp->options;
    if (!o)
        return;
    int cur_group = 0;
    while (!oend(o)) {
        if (h->n == h->cap) {
            unsigned cap = h->cap * 2 + 8;
            struct entry *ne = realloc(h->e, cap * sizeof *ne);
            if (!ne) {
                h->oom = 1;
                return;
            }
            h->e = ne;
            h->cap = cap;
        }
        struct entry *e = &h->e[h->n];
        e->opt = o;
        e->num = 0;
        e->shorts = (const char *)(uintptr_t)h->nshorts;  /* an offset until done */
        cur_group = o->group ? o->group : (!o->name && !o->key) ? cur_group + 1 : cur_group;
        e->group = cur_group;
        e->cluster = cl;
        e->argp = argp;
        e->ord = h->n;
        do {
            e->num++;
            if (oshort(o) && !(h->nshorts && memchr(h->shorts, o->key, h->nshorts))) {
                if (h->nshorts + 2 > h->shorts_cap) {
                    size_t cap = h->shorts_cap * 2 + 32;
                    char *ns = realloc(h->shorts, cap);
                    if (!ns) {
                        h->oom = 1;
                        return;
                    }
                    h->shorts = ns;
                    h->shorts_cap = cap;
                }
                h->shorts[h->nshorts++] = (char)o->key;
            }
            o++;
        } while (!oend(o) && oalias(o));
        h->n++;
    }
}

static void hol_build(struct hol *h, const struct argp *argp, struct cluster *cl)
{
    hol_entries(h, argp, cl);
    const struct argp_child *ch = argp->children;
    for (int i = 0; ch && ch[i].argp; i++) {
        struct cluster *sub = cl;
        if (ch[i].group || ch[i].header) {
            sub = calloc(1, sizeof *sub);
            if (!sub) {
                h->oom = 1;
                return;
            }
            sub->header = ch[i].header;
            sub->index = i;
            sub->group = ch[i].group;
            sub->parent = cl;
            sub->argp = argp;
            sub->depth = cl ? cl->depth + 1 : 0;
            sub->next = h->clusters;
            h->clusters = sub;
        }
        hol_build(h, ch[i].argp, sub);
    }
}

/* the short options of an entry are a NUL-free run: a terminated copy */
static int hol_finish(struct hol *h)
{
    if (h->oom)
        return -1;
    /* each entry's run, terminated, in a buffer of its own */
    size_t total = h->nshorts + h->n + 1;
    char *buf = malloc(total), *w = buf;
    if (!buf)
        return -1;
    for (unsigned i = 0; i < h->n; i++) {
        size_t from = (size_t)(uintptr_t)h->e[i].shorts;
        size_t to = i + 1 < h->n ? (size_t)(uintptr_t)h->e[i + 1].shorts : h->nshorts;
        h->e[i].shorts = w;
        memcpy(w, h->shorts + from, to - from);
        w += to - from;
        *w++ = 0;
    }
    *w = 0;
    free(h->shorts);
    h->shorts = buf;
    return 0;
}

static void hol_free(struct hol *h)
{
    free(h->e);
    free(h->shorts);
    while (h->clusters) {
        struct cluster *n = h->clusters->next;
        free(h->clusters);
        h->clusters = n;
    }
}

static struct entry *hol_find(struct hol *h, const char *name)
{
    for (unsigned i = 0; i < h->n; i++) {
        const struct argp_option *o = h->e[i].opt;
        for (unsigned k = 0; k < h->e[i].num; k++, o++)
            if (o->name && !ohidden(o) && !strcmp(o->name, name))
                return &h->e[i];
    }
    return 0;
}

/* the entry's first shown short option, or 0 */
static int first_short(const struct entry *e)
{
    const char *so = e->shorts;
    const struct argp_option *o = e->opt;
    for (unsigned k = 0; k < e->num; k++, o++)
        if (oshort(o) && *so == o->key) {
            if (!ohidden(o))
                return o->key;
            so++;
        }
    return 0;
}

static const char *first_long(const struct entry *e)
{
    const struct argp_option *o = e->opt;
    for (unsigned k = 0; k < e->num; k++, o++)
        if (o->name && !ohidden(o))
            return o->name;
    return 0;
}

/* group numbers: 0 and up first, ascending; then the negative ones,
 * ascending */
static int group_cmp(int a, int b, int eq)
{
    if (a == b)
        return eq;
    if ((a < 0) == (b < 0))
        return a - b;
    return b - a;
}

static const struct cluster *cluster_base(const struct cluster *c)
{
    while (c->parent)
        c = c->parent;
    return c;
}

static int cluster_cmp(const struct cluster *a, const struct cluster *b)
{
    while (a->depth > b->depth)
        a = a->parent;
    while (b->depth > a->depth)
        b = b->parent;
    while (a->parent != b->parent) {
        a = a->parent;
        b = b->parent;
    }
    return group_cmp(a->group, b->group, b->index - a->index);
}

static int is_within(const struct cluster *c, const struct cluster *outer)
{
    while (c && c != outer)
        c = c->parent;
    return c == outer;
}

/* a documentation option's name, past its blanks and dashes: and whether
 * it is no option (does not start with '-') */
static int doc_name(const char **name)
{
    while (isspace((unsigned char)**name))
        (*name)++;
    int nonopt = **name != '-';
    while (**name && !isalnum((unsigned char)**name))
        (*name)++;
    return nonopt;
}

/* the help's order: by group (a cluster's entries by its outermost
 * cluster's), entries outside clusters before those in them, clusters
 * among themselves, groups within a cluster; then documentation after
 * options, and by first letter ignoring case -- lower case first, and an
 * entry with no short option before one whose short option it is */
static int entry_cmp(const struct entry *a, const struct entry *b)
{
    int g1 = a->cluster ? cluster_base(a->cluster)->group : a->group;
    int g2 = b->cluster ? cluster_base(b->cluster)->group : b->group;
    if (g1 != g2)
        return group_cmp(g1, g2, 0);
    if (!a->cluster != !b->cluster)
        return a->cluster ? 1 : -1;
    if (a->cluster && a->cluster != b->cluster) {
        int c = cluster_cmp(a->cluster, b->cluster);
        if (c)
            return c;
    }
    if (a->group != b->group)
        return group_cmp(a->group, b->group, 0);
    int s1 = first_short(a), s2 = first_short(b);
    const char *l1 = first_long(a), *l2 = first_long(b);
    int d1 = odoc(a->opt) ? l1 && doc_name(&l1) : 0;
    int d2 = odoc(b->opt) ? l2 && doc_name(&l2) : 0;
    if (d1 != d2)
        return d1 - d2;            /* documentation after options */
    int c1 = s1 ? s1 : l1 ? (unsigned char)*l1 : 0;
    int c2 = s2 ? s2 : l2 ? (unsigned char)*l2 : 0;
    int lc = tolower(c1) - tolower(c2);
    if (lc)
        return lc;
    if (c1 != c2)
        return c2 - c1;            /* the same letter: lower case first */
    if (!s1 != !s2)
        return s1 ? 1 : -1;        /* the long option alone first */
    if (l1 && l2)
        return strcasecmp(l1, l2);
    return 0;
}

/* a stable merge sort, halves of n/2 and the rest */
static void sort_entries(struct entry *e, struct entry *tmp, unsigned n)
{
    if (n < 2)
        return;
    unsigned n1 = n / 2, n2 = n - n1;
    sort_entries(e, tmp, n1);
    sort_entries(e + n1, tmp, n2);
    unsigned i = 0, j = n1, k = 0;
    while (i < n1 && j < n)
        tmp[k++] = entry_cmp(&e[i], &e[j]) <= 0 ? e[i++] : e[j++];
    while (i < n1)
        tmp[k++] = e[i++];
    memcpy(e, tmp, k * sizeof *e);
}

/* ---- the help, written ---------------------------------------------------------------------- */

struct help {
    struct col *c;
    const struct argp_state *state;
    const struct entry *prev;
    int sep_groups, dup_note;
};

static void print_header(struct help *h, const char *str, const struct argp *argp)
{
    const char *f = filter(str, ARGP_KEY_HELP_HEADER, argp, h->state);
    if (f) {
        if (*f) {
            if (h->prev)
                col_putc(h->c, '\n');
            indent_to(h->c, up.header_col);
            col_set_lm(h->c, up.header_col);
            col_set_wm(h->c, up.header_col);
            col_puts(h->c, f);
            col_set_lm(h->c, 0);
            col_putc(h->c, '\n');
        }
        h->sep_groups = 1;
    }
    unfilter(f, str);
}

/* before an option's name: the blank line or header that starts a group
 * or cluster, for the entry's first; ", " between them */
static void before_name(struct help *h, const struct entry *e, int *first, int column)
{
    if (*first) {
        const struct entry *pe = h->prev;
        const struct cluster *cl = e->cluster;
        if (h->sep_groups && pe && e->group != pe->group)
            col_putc(h->c, '\n');
        if (cl && cl->header && *cl->header &&
            (!pe || (pe->cluster != cl && !is_within(pe->cluster, cl)))) {
            int wm = h->c->wm;
            print_header(h, cl->header, cl->argp);
            col_set_wm(h->c, wm);
        }
        *first = 0;
    } else {
        col_puts(h->c, ", ");
    }
    indent_to(h->c, column);
}

static void opt_arg(struct col *c, const struct argp_option *real, const char *req,
                    const char *opt)
{
    if (real->arg)
        col_printf(c, real->flags & OPTION_ARG_OPTIONAL ? opt : req, real->arg);
}

static void entry_help(struct help *h, const struct entry *e)
{
    struct col *c = h->c;
    const struct argp_option *real = e->opt, *o;
    int old_lm = col_set_lm(c, 0), old_wm = c->wm;
    int first = 1, has_long = 0;
    unsigned k;

    if (!odoc(real))
        for (o = real, k = 0; k < e->num; k++, o++)
            if (o->name && !ohidden(o)) {
                has_long = 1;
                break;
            }

    col_set_wm(c, up.short_opt_col);
    const char *so = e->shorts;
    for (o = real, k = 0; k < e->num; k++, o++)
        if (oshort(o) && o->key == *so) {
            if (!ohidden(o)) {
                before_name(h, e, &first, up.short_opt_col);
                col_putc(c, '-');
                col_putc(c, *so);
                if (!has_long || up.dup_args)
                    opt_arg(c, real, " %s", "[%s]");
                else if (real->arg)
                    h->dup_note = 1;
            }
            so++;
        }

    if (odoc(real)) {
        col_set_wm(c, up.doc_opt_col);
        for (o = real, k = 0; k < e->num; k++, o++)
            if (o->name && *o->name && !ohidden(o)) {
                before_name(h, e, &first, up.doc_opt_col);
                col_puts(c, o->name);
            }
    } else {
        col_set_wm(c, up.long_opt_col);
        for (o = real, k = 0; k < e->num; k++, o++)
            if (o->name && !ohidden(o)) {
                before_name(h, e, &first, up.long_opt_col);
                col_printf(c, "--%s", o->name);
                opt_arg(c, real, "=%s", "[=%s]");
            }
    }

    col_set_lm(c, 0);
    if (first) {
        if (oshort(real) || real->name)
            goto out;              /* every name shadowed or hidden */
        print_header(h, real->doc, e->argp);   /* a group header */
    } else {
        const char *f = filter(real->doc, real->key, e->argp, h->state);
        if (f && *f) {
            int at = col_point(c);
            col_set_lm(c, up.opt_doc_col);
            col_set_wm(c, up.opt_doc_col);
            if (at > up.opt_doc_col + 3)
                col_putc(c, '\n');
            else if (at >= up.opt_doc_col)
                col_puts(c, "   ");
            else
                indent_to(c, up.opt_doc_col);
            col_puts(c, f);
        }
        unfilter(f, real->doc);
        col_set_lm(c, 0);
        col_putc(c, '\n');
    }
    h->prev = e;
out:
    col_set_lm(c, old_lm);
    col_set_wm(c, old_wm);
}

static void hol_help(struct hol *hol, const struct argp_state *state, struct col *c)
{
    struct help h = { c, state, 0, 0, 0 };
    for (unsigned i = 0; i < hol->n; i++)
        entry_help(&h, &hol->e[i]);
    if (h.dup_note && up.dup_args_note) {
        static const char note[] =
            "Mandatory or optional arguments to long options are also mandatory or "
            "optional for any corresponding short options.";
        const char *f = filter(note, ARGP_KEY_HELP_DUP_ARGS_NOTE,
                               state ? state->root_argp : 0, state);
        if (f && *f) {
            col_putc(c, '\n');
            col_puts(c, f);
            col_putc(c, '\n');
        }
        unfilter(f, note);
    }
}

/* the usage line's options: [-abc] for the argumentless short ones,
 * then [-o ARG] each, then [--name] each */
static void hol_usage(struct hol *hol, struct col *c)
{
    if (!hol->n)
        return;
    char *flags = malloc(strlen(hol->shorts) + hol->n + 1), *w = flags;
    if (!flags)
        return;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < hol->n; i++) {
            const struct entry *e = &hol->e[i];
            const struct argp_option *o = e->opt, *real = e->opt;
            const char *so = e->shorts;
            for (unsigned k = 0; k < e->num; k++, o++) {
                if (!oshort(o) || *so != o->key)
                    continue;
                if (!oalias(o))
                    real = o;
                so++;
                if (ohidden(o))
                    continue;
                const char *arg = o->arg ? o->arg : real->arg;
                int fl = o->flags | real->flags;
                if (fl & OPTION_NO_USAGE)
                    continue;
                if (pass == 0) {
                    if (!arg)
                        *w++ = (char)o->key;
                } else if (arg) {
                    if (fl & OPTION_ARG_OPTIONAL) {
                        col_printf(c, " [-%c[%s]]", o->key, arg);
                    } else {
                        space_for(c, 6 + strlen(arg));
                        col_printf(c, "[-%c %s]", o->key, arg);
                    }
                }
            }
            if (pass == 0 && i + 1 == hol->n && w > flags) {
                *w = 0;
                col_printf(c, " [-%s]", flags);
            }
        }
    free(flags);
    for (unsigned i = 0; i < hol->n; i++) {
        const struct entry *e = &hol->e[i];
        const struct argp_option *o = e->opt, *real = e->opt;
        for (unsigned k = 0; k < e->num; k++, o++) {
            if (!o->name)
                continue;
            if (!oalias(o))
                real = o;
            if (ohidden(o))
                continue;
            const char *arg = o->arg ? o->arg : real->arg;
            int fl = o->flags | real->flags;
            if (fl & OPTION_NO_USAGE)
                continue;
            if (!arg)
                col_printf(c, " [--%s]", o->name);
            else if (fl & OPTION_ARG_OPTIONAL)
                col_printf(c, " [--%s[=%s]]", o->name, arg);
            else
                col_printf(c, " [--%s=%s]", o->name, arg);
        }
    }
}

static size_t args_levels(const struct argp *argp)
{
    size_t n = argp->args_doc && strchr(argp->args_doc, '\n');
    for (const struct argp_child *ch = argp->children; ch && ch->argp; ch++)
        n += args_levels(ch->argp);
    return n;
}

/* this argp's operands in the usage line, and its children's. An
 * args_doc of several lines gives a usage line for each: level says
 * which line each argp is at. Whether there is another line to print. */
static int args_usage(const struct argp *argp, const struct argp_state *state,
                      char **levels, int advance, struct col *c)
{
    char *mine = *levels;
    int multiple = 0;
    const char *doc = argp->args_doc;
    const char *f = filter(doc, ARGP_KEY_HELP_ARGS_DOC, argp, state);
    const char *nl = 0;
    if (f) {
        const char *cp = f;
        nl = strchrnul(cp, '\n');
        if (*nl) {
            multiple = 1;
            for (int i = 0; i < *mine; i++) {
                cp = nl + 1;
                nl = strchrnul(cp, '\n');
            }
            (*levels)++;
        }
        space_for(c, 1 + (size_t)(nl - cp));
        col_write(c, cp, (size_t)(nl - cp));
    }
    for (const struct argp_child *ch = argp->children; ch && ch->argp; ch++)
        advance = !args_usage(ch->argp, state, levels, advance, c);
    if (advance && multiple) {
        if (*nl) {
            (*mine)++;
            advance = 0;
        } else if (*mine > 0) {
            *mine = 0;
        }
    }
    unfilter(f, doc);
    return !advance;
}

/* the doc before the '\v' (pre) or after it (post), for this argp and,
 * with first_only, its children until one prints something */
static int argp_doc(const struct argp *argp, const struct argp_state *state, int post,
                    int pre_blank, int first_only, struct col *c)
{
    const char *text = 0;
    char *copy = 0;
    int anything = 0;
    if (argp->doc) {
        const char *vt = strchr(argp->doc, '\v');
        if (vt) {
            if (post) {
                text = vt + 1;
            } else {
                copy = strndup(argp->doc, (size_t)(vt - argp->doc));
                text = copy;
            }
        } else {
            text = post ? 0 : argp->doc;
        }
    }
    void *input = 0;
    const char *f = text;
    if (argp->help_filter) {
        input = _argp_input(argp, state);
        f = argp->help_filter(post ? ARGP_KEY_HELP_POST_DOC : ARGP_KEY_HELP_PRE_DOC, text,
                              input);
    }
    if (f) {
        if (pre_blank)
            col_putc(c, '\n');
        col_puts(c, f);
        if (col_point(c) > c->lm)
            col_putc(c, '\n');
        anything = 1;
    }
    unfilter(f, text);
    free(copy);
    if (post && argp->help_filter) {
        char *x = argp->help_filter(ARGP_KEY_HELP_EXTRA, 0, input);
        if (x) {
            if (anything || pre_blank)
                col_putc(c, '\n');
            col_puts(c, x);
            free(x);
            if (col_point(c) > c->lm)
                col_putc(c, '\n');
            anything = 1;
        }
    }
    for (const struct argp_child *ch = argp->children;
         ch && ch->argp && !(first_only && anything); ch++)
        anything |= argp_doc(ch->argp, state, post, anything || pre_blank, first_only, c);
    return anything;
}

static void help(const struct argp *argp, const struct argp_state *state, FILE *stream,
                 unsigned flags, const char *name)
{
    if (!stream)
        return;
    flockfile(stream);
    if (!up.valid)
        read_help_fmt(state);
    struct col c = { stream, 0, 0, 0, 0, 0, up.rmargin, 0, 0, 0 };
    struct hol hol;
    memset(&hol, 0, sizeof hol);
    int have_hol = 0, anything = 0;
    static const struct argp none = { 0 };
    if (!argp)
        argp = &none;

    if (flags & (ARGP_HELP_USAGE | ARGP_HELP_SHORT_USAGE | ARGP_HELP_LONG)) {
        hol_build(&hol, argp, 0);
        if (hol_finish(&hol)) {
            hol_free(&hol);
            funlockfile(stream);
            return;
        }
        have_hol = 1;
        struct entry *x = hol_find(&hol, "help");
        if (x)
            x->group = -1;
        if ((x = hol_find(&hol, "version")))
            x->group = -1;
        struct entry *tmp = malloc((hol.n ? hol.n : 1) * sizeof *tmp);
        if (tmp) {
            sort_entries(hol.e, tmp, hol.n);
            free(tmp);
        }
    }

    if (flags & (ARGP_HELP_USAGE | ARGP_HELP_SHORT_USAGE)) {
        size_t nlev = args_levels(argp);
        char *levels = calloc(nlev ? nlev : 1, 1);
        int first = 1, more;
        do {
            int old_wm = col_set_wm(&c, up.usage_indent);
            char *lv = levels;
            col_printf(&c, "%s %s", first ? "Usage:" : "  or: ", name);
            int old_lm = col_set_lm(&c, up.usage_indent);
            if (flags & ARGP_HELP_SHORT_USAGE) {
                if (hol.n)
                    col_puts(&c, " [OPTION...]");
            } else {
                hol_usage(&hol, &c);
                flags |= ARGP_HELP_SHORT_USAGE;
            }
            more = levels ? args_usage(argp, state, &lv, 1, &c) : 0;
            col_set_wm(&c, old_wm);
            col_set_lm(&c, old_lm);
            col_putc(&c, '\n');
            anything = 1;
            first = 0;
        } while (more);
        free(levels);
    }

    if (flags & ARGP_HELP_PRE_DOC)
        anything |= argp_doc(argp, state, 0, 0, 1, &c);

    if (flags & ARGP_HELP_SEE) {
        col_printf(&c, "Try `%s --help' or `%s --usage' for more information.\n", name, name);
        anything = 1;
    }

    if ((flags & ARGP_HELP_LONG) && hol.n) {
        if (anything)
            col_putc(&c, '\n');
        hol_help(&hol, state, &c);
        anything = 1;
    }

    if (flags & ARGP_HELP_POST_DOC)
        anything |= argp_doc(argp, state, 1, anything, 0, &c);

    if ((flags & ARGP_HELP_BUG_ADDR) && argp_program_bug_address) {
        if (anything)
            col_putc(&c, '\n');
        col_printf(&c, "Report bugs to %s.\n", argp_program_bug_address);
    }

    col_layout(&c);
    free(c.run);
    if (have_hol)
        hol_free(&hol);
    funlockfile(stream);
}

WEAK void argp_help(const struct argp *argp, FILE *stream, unsigned flags, char *name)
{
    help(argp, 0, stream, flags, name);
}

WEAK void argp_state_help(const struct argp_state *state, FILE *stream, unsigned flags)
{
    if ((state && (state->flags & ARGP_NO_ERRS)) || !stream)
        return;
    if (state && (state->flags & ARGP_LONG_ONLY))
        flags |= ARGP_HELP_LONG_ONLY;
    help(state ? state->root_argp : 0, state, stream, flags,
         state ? state->name : short_program_name());
    if (!state || !(state->flags & ARGP_NO_EXIT)) {
        if (flags & ARGP_HELP_EXIT_ERR)
            exit(argp_err_exit_status);
        if (flags & ARGP_HELP_EXIT_OK)
            exit(0);
    }
}

WEAK void argp_usage(const struct argp_state *state)
{
    argp_state_help(state, stderr, ARGP_HELP_STD_USAGE);
}

WEAK void argp_error(const struct argp_state *state, const char *fmt, ...)
{
    if (state && (state->flags & ARGP_NO_ERRS))
        return;
    FILE *f = state ? state->err_stream : stderr;
    if (!f)
        return;
    flockfile(f);
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%s: ", state ? state->name : short_program_name());
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    argp_state_help(state, f, ARGP_HELP_STD_ERR);
    funlockfile(f);
}

WEAK void argp_failure(const struct argp_state *state, int status, int errnum,
                       const char *fmt, ...)
{
    if (state && (state->flags & ARGP_NO_ERRS))
        return;
    FILE *f = state ? state->err_stream : stderr;
    if (!f)
        return;
    flockfile(f);
    fputs(state ? state->name : short_program_name(), f);
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        fputs(": ", f);
        vfprintf(f, fmt, ap);
        va_end(ap);
    }
    if (errnum) {
        fputs(": ", f);
        fputs(strerror(errnum), f);
    }
    fputc('\n', f);
    funlockfile(f);
    if (status && (!state || !(state->flags & ARGP_NO_EXIT)))
        exit(status);
}

/* ---- the options every program gets ------------------------------------------------------- */

enum { OPT_PROGNAME = -2, OPT_USAGE = -3, OPT_HANG = -4 };

static error_t default_parser(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case '?':
        argp_state_help(state, state->out_stream, ARGP_HELP_STD_HELP);
        return 0;
    case OPT_USAGE:
        argp_state_help(state, state->out_stream, ARGP_HELP_USAGE | ARGP_HELP_EXIT_OK);
        return 0;
    case OPT_PROGNAME: {
        program_invocation_name = arg;
        char *slash = strrchr(arg, '/');
        state->name = slash ? slash + 1 : arg;
        program_invocation_short_name = state->name;
        if ((state->flags & (ARGP_PARSE_ARGV0 | ARGP_NO_ERRS)) == ARGP_PARSE_ARGV0)
            state->argv[0] = arg;
        return 0;
    }
    case OPT_HANG: {
        int secs = atoi(arg ? arg : "3600");
        while (secs-- > 0)
            sleep(1);
        return 0;
    }
    }
    return ARGP_ERR_UNKNOWN;
}

static const struct argp_option default_options[] = {
    { "help", '?', 0, 0, "Give this help list", -1 },
    { "usage", OPT_USAGE, 0, 0, "Give a short usage message", 0 },
    { "program-name", OPT_PROGNAME, "NAME", OPTION_HIDDEN, "Set the program name", 0 },
    { "HANG", OPT_HANG, "SECS", OPTION_ARG_OPTIONAL | OPTION_HIDDEN,
      "Hang for SECS seconds (default 3600)", 0 },
    { 0, 0, 0, 0, 0, 0 },
};

static const struct argp default_argp = { default_options, default_parser, 0, 0, 0, 0, "libc" };

static error_t version_parser(int key, char *arg, struct argp_state *state)
{
    (void)arg;
    if (key != 'V')
        return ARGP_ERR_UNKNOWN;
    if (argp_program_version_hook)
        argp_program_version_hook(state->out_stream, state);
    else if (argp_program_version)
        fprintf(state->out_stream, "%s\n", argp_program_version);
    else
        argp_error(state, "%s", "(PROGRAM ERROR) No version known!?");
    if (!(state->flags & ARGP_NO_EXIT))
        exit(0);
    return 0;
}

static const struct argp_option version_options[] = {
    { "version", 'V', 0, 0, "Print program version", -1 },
    { 0, 0, 0, 0, 0, 0 },
};

static const struct argp version_argp = { version_options, version_parser, 0, 0, 0, 0, "libc" };

/* ---- parsing ---------------------------------------------------------------------------------- */

struct sizes { size_t shorts, longs, groups, inputs; };

static void count(const struct argp *argp, struct sizes *s)
{
    const struct argp_option *o = argp->options;
    if (o || argp->parser) {
        s->groups++;
        for (; o && !oend(o); o++) {
            s->shorts += 3;
            s->longs++;
        }
    }
    for (const struct argp_child *ch = argp->children; ch && ch->argp; ch++) {
        count(ch->argp, s);
        s->inputs++;
    }
}

/* the tables as getopt's: each argp with options or a parser a group */
static struct group *convert(struct parser *p, const struct argp *argp, struct group *parent,
                             unsigned parent_index, struct group *g, void ***inputs_end,
                             size_t *short_len)
{
    const struct argp_option *real = argp->options;
    if (real || argp->parser) {
        for (const struct argp_option *o = real; o && !oend(o); o++) {
            if (!oalias(o))
                real = o;
            if (odoc(real))
                continue;
            if (oshort(o)) {
                p->shorts[(*short_len)++] = (char)o->key;
                if (real->arg) {
                    p->shorts[(*short_len)++] = ':';
                    if (real->flags & OPTION_ARG_OPTIONAL)
                        p->shorts[(*short_len)++] = ':';
                }
                p->shorts[*short_len] = 0;
            }
            if (o->name) {
                int dup = 0;
                for (int i = 0; i < p->nlongs; i++)
                    if (!strcmp(p->longs[i].name, o->name))
                        dup = 1;
                if (!dup) {
                    struct option *lo = &p->longs[p->nlongs];
                    lo->name = o->name;
                    lo->has_arg = real->arg ? (real->flags & OPTION_ARG_OPTIONAL
                                                   ? optional_argument : required_argument)
                                            : no_argument;
                    lo->flag = 0;
                    lo->val = 256 + p->nlongs;
                    p->long_group[p->nlongs] = (int)(g - p->groups);
                    p->long_key[p->nlongs] = o->key ? o->key : real->key;
                    p->nlongs++;
                    memset(&p->longs[p->nlongs], 0, sizeof *lo);
                }
            }
        }
        g->parser = argp->parser;
        g->argp = argp;
        g->short_end = *short_len;
        g->args_processed = 0;
        g->parent = parent;
        g->parent_index = parent_index;
        g->input = g->hook = 0;
        g->child_inputs = 0;
        unsigned nch = 0;
        for (const struct argp_child *ch = argp->children; ch && ch->argp; ch++)
            nch++;
        if (nch) {
            g->child_inputs = *inputs_end;
            *inputs_end += nch;
        }
        parent = g++;
    } else {
        parent = 0;
    }
    unsigned i = 0;
    for (const struct argp_child *ch = argp->children; ch && ch->argp; ch++)
        g = convert(p, ch->argp, parent, i++, g, inputs_end, short_len);
    return g;
}

static error_t call(struct group *g, struct argp_state *st, int key, char *arg)
{
    if (!g->parser)
        return ARGP_ERR_UNKNOWN;
    st->hook = g->hook;
    st->input = g->input;
    st->child_inputs = g->child_inputs;
    st->arg_num = g->args_processed;
    error_t e = g->parser(key, arg, st);
    g->hook = st->hook;
    return e;
}

static error_t parse_arg(struct parser *p, char *val)
{
    int index = --p->state.next;
    error_t err = ARGP_ERR_UNKNOWN;
    struct group *g;
    int key = 0;
    for (g = p->groups; g < p->egroup && err == ARGP_ERR_UNKNOWN; g++) {
        p->state.next++;           /* ARGP_KEY_ARG consumes it */
        key = ARGP_KEY_ARG;
        err = call(g, &p->state, key, val);
        if (err == ARGP_ERR_UNKNOWN) {
            p->state.next--;       /* ARGP_KEY_ARGS starts at it */
            key = ARGP_KEY_ARGS;
            err = call(g, &p->state, key, 0);
        }
    }
    if (!err) {
        if (key == ARGP_KEY_ARGS)
            p->state.next = p->state.argc;
        if (p->state.next > index)
            (--g)->args_processed += (unsigned)(p->state.next - index);
        else
            p->try_getopt = 1;     /* the parser moved next back */
    }
    return err;
}

static error_t parse_opt(struct parser *p, int opt, char *val)
{
    error_t err = ARGP_ERR_UNKNOWN;
    if (opt < 256) {
        const char *at = strchr(p->shorts, opt);
        if (at)
            for (struct group *g = p->groups; g < p->egroup; g++)
                if (p->shorts + g->short_end > at) {
                    err = call(g, &p->state, opt, val);
                    break;
                }
        if (err == ARGP_ERR_UNKNOWN)
            argp_error(&p->state, "-%c: %s", opt,
                       "(PROGRAM ERROR) Option should have been recognized!?");
    } else {
        int i = opt - 256;
        err = call(&p->groups[p->long_group[i]], &p->state, p->long_key[i], val);
        if (err == ARGP_ERR_UNKNOWN)
            argp_error(&p->state, "--%s: %s", p->longs[i].name,
                       "(PROGRAM ERROR) Option should have been recognized!?");
    }
    return err;
}

/* one step; *arg_unknown says an ARGP_ERR_UNKNOWN came from running out
 * of arguments or from an operand no parser took, rather than an error */
static error_t parse_next(struct parser *p, int *arg_unknown)
{
    int opt;
    if (p->state.quoted && p->state.next < p->state.quoted)
        p->state.quoted = 0;       /* moved back before the "--" */
    if (p->try_getopt && !p->state.quoted) {
        p->go.ind = p->state.next;
        p->go.opt = -1;
        opt = __getopt_r(&p->go, p->state.argc, p->state.argv, p->shorts, p->longs, 0,
                         (p->state.flags & ARGP_LONG_ONLY) != 0);
        p->state.next = p->go.ind;
        if (opt == -1) {
            p->try_getopt = 0;
            if (p->state.next > 1 && !strcmp(p->state.argv[p->state.next - 1], "--"))
                p->state.quoted = p->state.next;
        } else if (opt == '?' && p->go.opt != -1) {
            *arg_unknown = 0;
            return ARGP_ERR_UNKNOWN;   /* getopt has said what was wrong */
        }
    } else {
        opt = -1;
    }
    if (opt == -1) {
        if (p->state.next >= p->state.argc || (p->state.flags & ARGP_NO_ARGS)) {
            *arg_unknown = 1;
            return ARGP_ERR_UNKNOWN;
        }
        opt = 1;
        p->go.arg = p->state.argv[p->state.next++];
    }
    error_t err = opt == 1 ? parse_arg(p, p->go.arg) : parse_opt(p, opt, p->go.arg);
    if (err == ARGP_ERR_UNKNOWN)
        *arg_unknown = opt == 1;
    return err;
}

static error_t finish(struct parser *p, error_t err, int arg_unknown, int *end_index)
{
    struct group *g;
    if (err == ARGP_ERR_UNKNOWN && arg_unknown)
        err = 0;
    if (!err) {
        if (p->state.next == p->state.argc) {
            for (g = p->groups; g < p->egroup && (!err || err == ARGP_ERR_UNKNOWN); g++)
                if (g->args_processed == 0)
                    err = call(g, &p->state, ARGP_KEY_NO_ARGS, 0);
            for (g = p->egroup - 1; g >= p->groups && (!err || err == ARGP_ERR_UNKNOWN); g--)
                err = call(g, &p->state, ARGP_KEY_END, 0);
            if (err == ARGP_ERR_UNKNOWN)
                err = 0;
            if (end_index)
                *end_index = p->state.next;
        } else if (end_index) {
            *end_index = p->state.next;
        } else {
            if (!(p->state.flags & ARGP_NO_ERRS) && p->state.err_stream)
                fprintf(p->state.err_stream, "%s: Too many arguments\n", p->state.name);
            err = ARGP_ERR_UNKNOWN;
        }
    }
    if (err) {
        if (err == ARGP_ERR_UNKNOWN)
            argp_state_help(&p->state, p->state.err_stream, ARGP_HELP_STD_ERR);
        for (g = p->groups; g < p->egroup; g++)
            call(g, &p->state, ARGP_KEY_ERROR, 0);
    } else {
        for (g = p->egroup - 1; g >= p->groups && (!err || err == ARGP_ERR_UNKNOWN); g--)
            err = call(g, &p->state, ARGP_KEY_SUCCESS, 0);
        if (err == ARGP_ERR_UNKNOWN)
            err = 0;
    }
    for (g = p->egroup - 1; g >= p->groups; g--)
        call(g, &p->state, ARGP_KEY_FINI, 0);
    return err == ARGP_ERR_UNKNOWN ? EINVAL : err;
}

WEAK error_t argp_parse(const struct argp *argp, int argc, char **argv, unsigned flags,
                        int *end_index, void *input)
{
    struct argp_child kids[4];
    struct argp top;
    if (!(flags & ARGP_NO_HELP)) {
        /* the program's options and the standard ones, as two children of
         * an argp of no options */
        memset(kids, 0, sizeof kids);
        memset(&top, 0, sizeof top);
        int k = 0;
        if (argp)
            kids[k++].argp = argp;
        kids[k++].argp = &default_argp;
        if (argp_program_version || argp_program_version_hook)
            kids[k++].argp = &version_argp;
        top.children = kids;
        argp = &top;
    }

    struct sizes sz = { (flags & ARGP_NO_ARGS) ? 0 : 1, 0, 0, 0 };
    if (argp)
        count(argp, &sz);
    struct parser p;
    memset(&p, 0, sizeof p);
    size_t gbytes = (sz.groups + 1) * sizeof(struct group);
    size_t ibytes = sz.inputs * sizeof(void *);
    size_t lbytes = (sz.longs + 1) * sizeof(struct option);
    size_t kbytes = 2 * (sz.longs + 1) * sizeof(int);
    char *mem = calloc(1, gbytes + ibytes + lbytes + kbytes + sz.shorts + 2);
    if (!mem)
        return ENOMEM;
    p.groups = (struct group *)mem;
    p.child_inputs = (void **)(mem + gbytes);
    p.longs = (struct option *)(mem + gbytes + ibytes);
    p.long_group = (int *)(mem + gbytes + ibytes + lbytes);
    p.long_key = p.long_group + sz.longs + 1;
    p.shorts = mem + gbytes + ibytes + lbytes + kbytes;

    size_t slen = 0;
    if (flags & ARGP_IN_ORDER)
        p.shorts[slen++] = '-';
    else if (flags & ARGP_NO_ARGS)
        p.shorts[slen++] = '+';
    p.shorts[slen] = 0;
    p.argp = argp;
    void **inputs_end = p.child_inputs;
    p.egroup = argp ? convert(&p, argp, 0, 0, p.groups, &inputs_end, &slen) : p.groups;

    p.state.root_argp = argp;
    p.state.argc = argc;
    p.state.argv = argv;
    p.state.flags = flags;
    p.state.err_stream = stderr;
    p.state.out_stream = stdout;
    p.state.next = 0;
    p.state.pstate = &p;
    p.try_getopt = 1;
    p.go.ind = 0;

    error_t err = 0;
    if (p.groups < p.egroup)
        p.groups->input = input;
    for (struct group *g = p.groups; g < p.egroup && (!err || err == ARGP_ERR_UNKNOWN); g++) {
        if (g->parent)
            g->input = g->parent->child_inputs[g->parent_index];
        if (!g->parser && g->argp->children && g->argp->children->argp)
            g->child_inputs[0] = g->input;
        err = call(g, &p.state, ARGP_KEY_INIT, 0);
    }
    if (err == ARGP_ERR_UNKNOWN)
        err = 0;
    if (err) {
        free(mem);
        return err;
    }

    if (flags & ARGP_NO_ERRS) {
        p.go.err = 0;
        if (flags & ARGP_PARSE_ARGV0) {
            p.state.argv--;        /* getopt skips argv[0]: give it a dummy */
            p.state.argc++;
        }
    } else {
        p.go.err = 1;
    }
    if (p.state.argv == argv && argv[0]) {
        char *slash = strrchr(argv[0], '/');
        p.state.name = slash ? slash + 1 : argv[0];
    } else {
        p.state.name = (char *)short_program_name();
    }

    int arg_unknown = 0;
    while (!err)
        err = parse_next(&p, &arg_unknown);
    err = finish(&p, err, arg_unknown, end_index);
    free(mem);
    return err;
}
