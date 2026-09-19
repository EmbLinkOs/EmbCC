/* Itanium C++ ABI name mangling (both targets use it) — what makes EmbCC's
 * objects link with g++'s and with libstdc++. The part that is easy to get
 * wrong is substitution: every substitutable component (a name prefix, a
 * class or enum name, a qualified or compound type) is numbered in the order
 * it is mangled, and a repeat is written S_, S0_, S1_... instead. The
 * function's own final name is never a candidate; builtin types are not
 * either, though a cv-qualified one is. */
#include "cxx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../arch/target.h"
#include "../driver/util.h"

struct mbuf {
    char *p;
    size_t len, cap;
    const char *subs[256];     /* substitution candidates, by index */
    int nsubs;
};

static void put(struct mbuf *m, const char *s)
{
    size_t n = strlen(s);
    if (m->len + n + 1 > m->cap) {
        m->cap = (m->len + n + 1) * 2;
        m->p = xrealloc(m->p, m->cap);
    }
    memcpy(m->p + m->len, s, n + 1);
    m->len += n;
}

/* An unnamed class's or enum's name (its number in its scope, from 1),
 * written Ut_, Ut0_, Ut1_, ... (5.1.6's <unnamed-type-name>) */
static const char *unnamed(int no)
{
    return cx_fmt("\3%d", no);
}

static void put_source(struct mbuf *m, const char *id)
{
    if (id[0] == '\3') {
        int no = atoi(id + 1);
        char u[24];
        if (no <= 1)
            snprintf(u, sizeof u, "Ut_");
        else
            snprintf(u, sizeof u, "Ut%d_", no - 2);
        put(m, u);
        return;
    }
    char b[16];
    snprintf(b, sizeof b, "%zu", strlen(id));
    put(m, b);
    put(m, id);
}

static int sub_find(struct mbuf *m, const char *s)
{
    for (int i = 0; i < m->nsubs; i++)
        if (strcmp(m->subs[i], s) == 0)
            return i;
    return -1;
}

static void sub_add(struct mbuf *m, const char *s)
{
    if (m->nsubs < 256 && sub_find(m, s) < 0)
        m->subs[m->nsubs++] = s;
}

/* S_ for index 0, then S0_, S1_, ... S9_, SA_ .. SZ_, S10_ (base 36). */
static void put_sub(struct mbuf *m, int i)
{
    if (i == 0) {
        put(m, "S_");
        return;
    }
    char digits[16];
    int n = 0, v = i - 1;
    do {
        int d = v % 36;
        digits[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v /= 36;
    } while (v);
    char b[20];
    int k = 0;
    b[k++] = 'S';
    while (n) b[k++] = digits[--n];
    b[k++] = '_';
    b[k] = 0;
    put(m, b);
}

/* Mangle `text` (an already-built component) through the substitution
 * table: its S-reference if seen, else the text itself (then recorded). */
static void put_component(struct mbuf *m, const char *text)
{
    int i = sub_find(m, text);
    if (i >= 0) {
        put_sub(m, i);
        return;
    }
    put(m, text);
    sub_add(m, text);
}

static int is_std(struct cscope *s)
{
    return s && s->k == SC_NAMESPACE && s->name &&
           strcmp(s->name, "std") == 0 && s->parent == cx_global;
}

/* The chain of named scopes enclosing (and including) `s`, outermost first,
 * stopping at the global namespace. Anonymous namespaces mangle as
 * `12_GLOBAL__N_1`. */
static int scope_chain(struct cscope *s, struct cscope **out, int max)
{
    struct cscope *tmp[64];
    int n = 0;
    for (; s && s != cx_global && n < 64; s = s->parent)
        if (s->k == SC_NAMESPACE || s->k == SC_CLASS)
            tmp[n++] = s;
    int k = 0;
    while (n && k < max)
        out[k++] = tmp[--n];
    return k;
}

static const char *scope_ident(struct cscope *s)
{
    if (s->k == SC_NAMESPACE && s->anon)
        return "_GLOBAL__N_1";
    if (s->k == SC_CLASS && !s->name && s->cls)
        return unnamed(s->cls->unnamed_no);
    return s->name;
}

static void mangle_type(struct mbuf *m, struct cty *t);
/* A template-id's arguments as written, those for a trailing parameter
 * pack gathered into one (J ... E), as an instance's are. */
static struct ctarg *tid_args(struct cty *t, int *n)
{
    struct ctemplate *tm = t->tmpl;
    int pi = -1;
    for (int i = 0; i < tm->nparams; i++)
        if (tm->params[i].pack)
            pi = i;
    if (pi >= 0 && pi != tm->nparams - 1)
        return t->targs;
    if (tm->tparam && tm->tt_pack)
        pi = tm->tt_pack - 1;
    if (pi < 0 || *n < pi)
        return t->targs;
    if (*n == pi + 1 && t->targs[pi].is_pack && !t->targs[pi].expansion)
        return t->targs;
    struct ctarg *r = xcalloc((size_t)(pi + 1), sizeof *r);
    memcpy(r, t->targs, (size_t)pi * sizeof *r);
    r[pi].kind = tm->params ? tm->params[pi].kind : t->targs[pi].kind;
    r[pi].is_pack = 1;
    r[pi].elems = t->targs + pi;
    r[pi].nelems = *n - pi;
    *n = pi + 1;
    return r;
}

static const char *type_key(struct cty *t);
static const char *targs_key(struct ctarg *a, int n);
static void put_targs(struct mbuf *m, struct ctarg *a, int n);

/* A nested name as steps: each a source name, or the template arguments
 * following the name before it. Every step's cumulative spelling is a
 * substitution candidate (the template-prefix, then with its arguments),
 * keyed without substitutions so a repeat is found however it was first
 * written. */
struct step {
    const char *key;
    const char *name;         /* a source name, or NULL: arguments */
    struct ctarg *args;
    int nargs;
    const char *abbr;         /* a standard abbreviation written instead
                               * (Sa, So, ...): no substitution candidate */
};

static int add_step(struct step *st, int n, const char *prev,
                    const char *name, struct ctarg *args, int nargs,
                    int is_template)
{
    const char *k = cx_fmt("%s%zu%s", prev, strlen(name), name);
    st[n].key = k;
    st[n].name = name;
    st[n].args = NULL;
    st[n].nargs = 0;
    st[n].abbr = NULL;
    n++;
    if (is_template) {
        st[n].key = cx_fmt("%sI%sE", k, targs_key(args, nargs));
        st[n].name = NULL;
        st[n].args = args;
        st[n].nargs = nargs;
        st[n].abbr = NULL;
        n++;
    }
    return n;
}

/* Is class argument a std::X<char, ...> as the abbreviations mean it? */
static int std_class_is(struct ctarg *a, const char *name)
{
    if (a->kind != TP_TYPE || a->is_pack || !a->type ||
        a->type->k != CT_CLASS || a->type->q)
        return 0;
    struct cclass *c = a->type->cls;
    if (!c->name || strcmp(c->name, name) != 0 || !c->owner ||
        !is_std(c->owner) || c->ntargs != 1 || c->targs[0].kind != TP_TYPE)
        return 0;
    struct cty *t = c->targs[0].type;
    return t->k == CT_CHAR && !t->q;
}

static int is_char_arg(struct ctarg *a)
{
    return a->kind == TP_TYPE && !a->is_pack && a->type &&
           a->type->k == CT_CHAR && !a->type->q;
}

/* The Itanium ABI's standard abbreviations (5.1.8): a class of ::std named
 * `std::allocator<...>` is SaI...E, `std::basic_string<...>` SbI...E,
 * basic_string<char, char_traits<char>, allocator<char>> Ss, and
 * basic_istream / basic_ostream / basic_iostream<char, char_traits<char>>
 * Si / So / Sd. 0 when none applies; else the steps written (from n). */
static int std_abbrev(struct cclass *c, struct step *st, int n)
{
    if (!c->name || !c->tmpl)
        return 0;
    const char *whole = NULL, *prefix = NULL;
    if (!strcmp(c->name, "allocator"))
        prefix = "Sa";
    else if (!strcmp(c->name, "basic_string")) {
        prefix = "Sb";
        if (c->ntargs == 3 && is_char_arg(&c->targs[0]) &&
            std_class_is(&c->targs[1], "char_traits") &&
            std_class_is(&c->targs[2], "allocator"))
            whole = "Ss";
    } else if (c->ntargs == 2 && is_char_arg(&c->targs[0]) &&
               std_class_is(&c->targs[1], "char_traits")) {
        whole = !strcmp(c->name, "basic_istream") ? "Si"
                : !strcmp(c->name, "basic_ostream") ? "So"
                : !strcmp(c->name, "basic_iostream") ? "Sd" : NULL;
    }
    if (whole) {
        st[n].key = whole;
        st[n].name = whole;
        st[n].args = NULL;
        st[n].nargs = 0;
        st[n].abbr = whole;
        return n + 1;
    }
    if (!prefix)
        return 0;
    st[n].key = prefix;
    st[n].name = prefix;
    st[n].args = NULL;
    st[n].nargs = 0;
    st[n].abbr = prefix;
    n++;
    st[n].key = cx_fmt("%sI%sE", prefix, targs_key(c->targs, c->ntargs));
    st[n].name = NULL;
    st[n].args = c->targs;
    st[n].nargs = c->ntargs;
    st[n].abbr = NULL;
    return n + 1;
}

/* The steps naming scope s (namespaces and classes, outermost first); *std
 * is set when the outermost is ::std (written St, itself no candidate). */
static int scope_steps(struct cscope *s, struct step *st, int *std)
{
    struct cscope *chain[64];
    int n = scope_chain(s, chain, 64), k = 0, i = 0;
    const char *prev = "";
    *std = n > 0 && is_std(chain[0]);
    if (*std) {
        prev = "St";
        i = 1;
    }
    for (; i < n && k < 120; i++) {
        struct cscope *c = chain[i];
        struct cclass *cls = c->k == SC_CLASS ? c->cls : NULL;
        if (cls && *std && k == 0) {
            int a = std_abbrev(cls, st, 0);
            if (a) {                  /* std::basic_ostream<char>::... */
                k = a;
                *std = 0;
                prev = st[k - 1].key;
                continue;
            }
        }
        k = add_step(st, k, prev, scope_ident(c),
                     cls ? cls->targs : NULL, cls ? cls->ntargs : 0,
                     cls && cls->tmpl);
        prev = st[k - 1].key;
    }
    return k;
}

/* Writes steps [0, n): the longest prefix already a candidate as its
 * S-reference, then the rest, each added. The last `nocand` steps are not
 * candidates (a function's own name). */
static void put_steps(struct mbuf *m, struct step *st, int n, int nocand)
{
    int from = 0;
    for (int j = n - 1 - nocand; j >= 0; j--) {
        if (st[j].abbr)
            continue;
        int i = sub_find(m, st[j].key);
        if (i >= 0) {
            put_sub(m, i);
            from = j + 1;
            break;
        }
    }
    for (int j = from; j < n; j++) {
        if (st[j].abbr) {
            put(m, st[j].abbr);       /* (no candidate) */
            continue;
        }
        if (st[j].name)
            put_source(m, st[j].name);
        else
            put_targs(m, st[j].args, st[j].nargs);
        if (j < n - nocand)
            sub_add(m, st[j].key);
    }
}

/* A name made of scope steps plus the entity's own: `3Foo`, `St3Foo`,
 * `1AIiE`, `N3foo3BarE`, `N2ns3BoxIlE2InE` — or one substitution. */
static void put_name(struct mbuf *m, struct step *st, int n, int std,
                     int nocand)
{
    int last_name = n - 1;
    while (last_name > 0 && !st[last_name].name)
        last_name--;
    int nested = last_name > 0;             /* more than one component */
    if (!nocand && !st[n - 1].abbr) {
        int i = sub_find(m, st[n - 1].key);
        if (i >= 0) {
            put_sub(m, i);
            return;
        }
    }
    if (!nested) {
        if (std)
            put(m, "St");
        put_steps(m, st, n, nocand);
        return;
    }
    put(m, "N");
    if (std)
        put(m, "St");
    put_steps(m, st, n, nocand);
    put(m, "E");
}

static int class_steps(struct cclass *c, struct step *st, int *std)
{
    int n = scope_steps(c->owner, st, std);
    if (*std && n == 0) {
        int k = std_abbrev(c, st, 0);
        if (k) {
            *std = 0;                   /* (the abbreviation says std) */
            return k;
        }
    }
    const char *prev = n ? st[n - 1].key : *std ? "St" : "";
    return add_step(st, n, prev, c->name ? c->name : unnamed(c->unnamed_no),
                    c->targs,
                    c->ntargs, c->tmpl != NULL);
}

static const char *type_name_key(struct cscope *owner, const char *name)
{
    struct step st[128];
    int std;
    int n = scope_steps(owner, st, &std);
    const char *prev = n ? st[n - 1].key : std ? "St" : "";
    n = add_step(st, n, prev, name, NULL, 0, 0);
    return st[n - 1].key;
}

/* A class or enum name — or its substitution when it was seen already. */
static void put_class_name(struct mbuf *m, struct cclass *c)
{
    struct step st[128];
    int std;
    int n = class_steps(c, st, &std);
    put_name(m, st, n, std, 0);
}

static void put_enum_name(struct mbuf *m, struct cenum *en)
{
    struct step st[128];
    int std;
    int n = scope_steps(en->owner, st, &std);
    const char *prev = n ? st[n - 1].key : std ? "St" : "";
    n = add_step(st, n, prev, en->name ? en->name : unnamed(en->unnamed_no),
                 NULL, 0, 0);
    put_name(m, st, n, std, 0);
}

/* A template's name (a template template argument, a CT_TID's). */
static void put_template_name(struct mbuf *m, struct ctemplate *t)
{
    struct step st[128];
    int std;
    int n = scope_steps(t->scope, st, &std);
    const char *prev = n ? st[n - 1].key : std ? "St" : "";
    n = add_step(st, n, prev, t->name, NULL, 0, 0);
    put_name(m, st, n, std, 0);
}

static const char *class_key(struct cclass *c)
{
    struct step st[128];
    int std;
    int n = class_steps(c, st, &std);
    return st[n - 1].key;
}

static const char *builtin_code(enum cty_kind k)
{
    switch (k) {
    case CT_VOID: return "v";
    case CT_BOOL: return "b";
    case CT_CHAR: return "c";
    case CT_SCHAR: return "a";
    case CT_UCHAR: return "h";
    case CT_WCHAR: return "w";
    case CT_CHAR8: return "Du";
    case CT_CHAR16: return "Ds";
    case CT_CHAR32: return "Di";
    case CT_SHORT: return "s";
    case CT_USHORT: return "t";
    case CT_INT: return "i";
    case CT_UINT: return "j";
    case CT_LONG: return "l";
    case CT_ULONG: return "m";
    case CT_LLONG: return "x";
    case CT_ULLONG: return "y";
    case CT_FLOAT: return "f";
    case CT_DOUBLE: return "d";
    case CT_LDOUBLE: return "e";
    case CT_NULLPTR: return "Dn";
    case CT_AUTO: return "Da";         /* a deduced type as declared */
    default: return NULL;
    }
}

/* t's key in the substitution table: an encoding with no substitutions in
 * it, so a repeat is recognized however its first occurrence was written
 * (its own parts may have been abbreviated there). */
static const char *type_key(struct cty *t)
{
    if (t->q && t->k != CT_FUNC)
        return cx_fmt("%s%s%s", (t->q & CQ_VOLATILE) ? "V" : "",
                      (t->q & CQ_CONST) ? "K" : "", type_key(ct_unqual(t)));
    const char *b = t->dauto ? "Dc" : builtin_code(t->k);
    if (b)
        return b;
    switch (t->k) {
    case CT_PTR: return cx_fmt("P%s", type_key(t->to));
    case CT_COMPLEX: return cx_fmt("C%s", type_key(t->to));
    case CT_LREF: return cx_fmt("R%s", type_key(t->to));
    case CT_RREF: return cx_fmt("O%s", type_key(t->to));
    case CT_ARRAY:
        if (t->n == -2)
            return cx_fmt("AT%d__%s", t->bparam, type_key(t->to));
        return t->n >= 0 ? cx_fmt("A%ld_%s", t->n, type_key(t->to))
                         : cx_fmt("A_%s", type_key(t->to));
    case CT_FUNC: {
        char *k = cx_fmt("%s%sF%s", (t->fq & CQ_VOLATILE) ? "V" : "",
                         (t->fq & CQ_CONST) ? "K" : "", type_key(t->to));
        for (int i = 0; i < t->np; i++)
            k = cx_fmt("%s%s", k, type_key(t->params[i]));
        return cx_fmt("%s%s%s%sE", k, t->np == 0 && !t->variadic ? "v" : "",
                      t->variadic ? "z" : "",
                      t->refq == 1 ? "R" : t->refq == 2 ? "O" : "");
    }
    case CT_MPTR:
        return cx_fmt("M%s%s", type_key(t->mclass ? t->mclass
                                                  : ct_class(t->cls)),
                      type_key(t->to));
    case CT_CLASS:
        return class_key(t->cls);
    case CT_ENUM:
        return type_name_key(t->en->owner, t->en->name ? t->en->name
                                           : unnamed(t->en->unnamed_no));
    case CT_TPARAM:
        return cx_fmt("T%ld_", t->n);
    case CT_TID: {
        struct step st[128];
        int std, na = t->ntargs;
        struct ctarg *a = tid_args(t, &na);
        if (t->tmpl->tparam)
            return cx_fmt("T%d_I%sE", t->tmpl->tparam - 1, targs_key(a, na));
        int n = scope_steps(t->tmpl->scope, st, &std);
        const char *prev = n ? st[n - 1].key : std ? "St" : "";
        n = add_step(st, n, prev, t->tmpl->name, a, na, 1);
        return st[n - 1].key;
    }
    case CT_DEP: {
        if (t->dexpr)
            return cx_fmt("DT%sE", mexpr_key(t->dexpr));
        char *k = cx_fmt("N%s", t->to ? type_key(t->to) : "?");
        for (int i = 0; i < t->ndnames; i++)
            k = cx_fmt("%s%zu%s", k, strlen(t->dnames[i]), t->dnames[i]);
        return cx_fmt("%sE", k);
    }
    default:
        return "?";
    }
}

static void put_mexpr(struct mbuf *m, const char *x);

static void mangle_type(struct mbuf *m, struct cty *t)
{
    /* cv-qualified: [V][K] then the unqualified type; the qualified type as
     * a whole is a candidate (after its parts) */
    if (t->q && t->k != CT_FUNC) {
        const char *key = type_key(t);
        int i = sub_find(m, key);
        if (i >= 0) {
            put_sub(m, i);
            return;
        }
        if (t->q & CQ_VOLATILE) put(m, "V");
        if (t->q & CQ_CONST) put(m, "K");
        mangle_type(m, ct_unqual(t));
        sub_add(m, key);
        return;
    }
    const char *b = t->dauto ? "Dc" : builtin_code(t->k);
    if (b) {
        put(m, b);
        return;
    }
    switch (t->k) {
    case CT_PTR: case CT_LREF: case CT_RREF: case CT_ARRAY: case CT_FUNC:
    case CT_MPTR: case CT_COMPLEX: {
        const char *key = type_key(t);
        int i = sub_find(m, key);
        if (i >= 0) {
            put_sub(m, i);
            return;
        }
        /* written out: its parts are numbered first, then the whole */
        switch (t->k) {
        case CT_PTR: put(m, "P"); mangle_type(m, t->to); break;
        case CT_COMPLEX: put(m, "C"); mangle_type(m, t->to); break;
        case CT_LREF: put(m, "R"); mangle_type(m, t->to); break;
        case CT_RREF: put(m, "O"); mangle_type(m, t->to); break;
        case CT_ARRAY: {
            char n[32];
            if (t->n == -2) {
                /* T (&)[N]: the bound is the template parameter */
                put(m, "A");
                mangle_type(m, ct_tparam(t->bparam, NULL));
                put(m, "_");
                mangle_type(m, t->to);
                break;
            }
            if (t->n >= 0) snprintf(n, sizeof n, "A%ld_", t->n);
            else snprintf(n, sizeof n, "A_");
            put(m, n);
            mangle_type(m, t->to);
            break;
        }
        case CT_MPTR:
            put(m, "M");
            mangle_type(m, t->mclass ? t->mclass : ct_class(t->cls));
            mangle_type(m, t->to);
            break;
        default:
            /* a member function's cv and ref-qualifier are inside its
             * function type: one candidate, KFvvE */
            if (t->fq & CQ_VOLATILE) put(m, "V");
            if (t->fq & CQ_CONST) put(m, "K");
            put(m, "F");
            mangle_type(m, t->to);
            if (t->np == 0 && !t->variadic)
                put(m, "v");
            for (int i2 = 0; i2 < t->np; i2++)
                mangle_type(m, t->params[i2]);
            if (t->variadic)
                put(m, "z");
            if (t->refq)
                put(m, t->refq == 1 ? "R" : "O");
            put(m, "E");
        }
        sub_add(m, key);
        return;
    }
    case CT_CLASS:
        put_class_name(m, t->cls);
        return;
    case CT_ENUM:
        put_enum_name(m, t->en);
        return;
    case CT_TPARAM: {
        /* T_, T0_, T1_ ...: a template parameter, itself a candidate */
        const char *key = type_key(t);
        int i = sub_find(m, key);
        if (i >= 0) {
            put_sub(m, i);
            return;
        }
        put(m, t->n == 0 ? "T_" : cx_fmt("T%ld_", t->n - 1));
        sub_add(m, key);
        return;
    }
    case CT_TID: {
        struct step st[128];
        int std, na = t->ntargs;
        struct ctarg *a = tid_args(t, &na);
        if (t->tmpl->tparam) {
            /* TT<args>: T_ I ... E, both candidates */
            const char *key = type_key(t);
            int i = sub_find(m, key);
            if (i >= 0) {
                put_sub(m, i);
                return;
            }
            struct cty *tp = ct_tparam(t->tmpl->tparam - 1, t->tmpl->name);
            mangle_type(m, tp);
            put_targs(m, a, na);
            sub_add(m, key);
            return;
        }
        int n = scope_steps(t->tmpl->scope, st, &std);
        const char *prev = n ? st[n - 1].key : std ? "St" : "";
        n = add_step(st, n, prev, t->tmpl->name, a, na, 1);
        put_name(m, st, n, std, 0);
        return;
    }
    case CT_DEP: {
        /* typename T::a::b: N T_ 1a 1b E */
        const char *key = type_key(t);
        int i = sub_find(m, key);
        if (i >= 0) {
            put_sub(m, i);
            return;
        }
        if (t->dexpr) {
            /* decltype(e): DT <expression> E */
            put(m, "DT");
            put_mexpr(m, t->dexpr);
            put(m, "E");
            sub_add(m, key);
            return;
        }
        put(m, "N");
        if (t->to)
            mangle_type(m, t->to);
        for (int k = 0; k < t->ndnames; k++)
            put_source(m, t->dnames[k]);
        put(m, "E");
        sub_add(m, key);
        return;
    }
    case CT_VALIST:
        /* g++'s own spelling: x86-64's va_list is __va_list_tag[1], which a
         * parameter decays to a pointer; aarch64's is struct std::__va_list */
        if (target_get() == TARGET_AARCH64) {
            put_component(m, "St9__va_list");
        } else {
            int i = sub_find(m, "P13__va_list_tag");
            if (i >= 0) {
                put_sub(m, i);
                return;
            }
            put(m, "P");
            put_component(m, "13__va_list_tag");
            sub_add(m, "P13__va_list_tag");
        }
        return;
    default:
        put(m, "?");
    }
}

/* ---- template arguments ---- */

/* Types referenced from expression manglings (\1 index \1). */
static struct cty **mtypes;
static int nmtypes, capmtypes;

int mangle_type_ref(struct cty *t)
{
    if (nmtypes == capmtypes) {
        capmtypes = capmtypes ? capmtypes * 2 : 64;
        mtypes = xrealloc(mtypes, (size_t)capmtypes * sizeof *mtypes);
    }
    mtypes[nmtypes] = t;
    return nmtypes++;
}

/* An expression's mangling with its types written into m (keys: into a
 * substitution-free key). */
static void put_mexpr(struct mbuf *m, const char *x)
{
    while (*x) {
        if (*x == 1) {
            int i = 0;
            for (x++; *x != 1; x++)
                i = i * 10 + (*x - '0');
            x++;
            mangle_type(m, mtypes[i]);
            continue;
        }
        if (*x == 2) {                      /* a parameter, as an operand */
            int i = 0;
            for (x++; *x != 2; x++)
                i = i * 10 + (*x - '0');
            x++;
            put(m, i == 0 ? "T_" : cx_fmt("T%d_", i - 1));
            continue;
        }
        char c[2] = { *x++, 0 };
        put(m, c);
    }
}

const char *mexpr_key(const char *x)
{
    char *k = "";
    while (*x) {
        if (*x == 1) {
            int i = 0;
            for (x++; *x != 1; x++)
                i = i * 10 + (*x - '0');
            x++;
            k = cx_fmt("%s%s", k, type_key(mtypes[i]));
            continue;
        }
        if (*x == 2) {
            int i = 0;
            for (x++; *x != 2; x++)
                i = i * 10 + (*x - '0');
            x++;
            k = cx_fmt("%sT%d_", k, i);
            continue;
        }
        k = cx_fmt("%s%c", k, *x++);
    }
    return k;
}

static const char *value_code(struct cty *t)
{
    const char *b = t ? builtin_code(ct_unqual(t)->k) : NULL;
    return b ? b : "i";
}

static const char *targs_key(struct ctarg *a, int n)
{
    char *k = "";
    for (int i = 0; i < n; i++) {
        struct ctarg *x = &a[i];
        if (x->expansion && x->elems[0].kind == TP_VALUE)
            k = cx_fmt("%sXsp%sE", k, x->elems[0].mexpr
                                      ? mexpr_key(x->elems[0].mexpr)
                                      : type_key(x->elems[0].vtype));
        else if (x->expansion)
            k = cx_fmt("%sDp%s", k, targs_key(x->elems, 1));
        else if (x->is_pack)
            k = cx_fmt("%sJ%sE", k, targs_key(x->elems, x->nelems));
        else if (x->kind == TP_TYPE)
            k = cx_fmt("%s%s", k, type_key(x->type));
        else if (x->kind == TP_VALUE && x->mexpr)
            k = cx_fmt("%sX%sE", k, mexpr_key(x->mexpr));
        else if (x->kind == TP_VALUE)
            k = x->vtype && x->vtype->k == CT_TPARAM
                ? cx_fmt("%sXT%ld_E", k, x->vtype->n)
                : cx_fmt("%sL%s%s%ldE", k, value_code(x->vtype),
                         x->value < 0 ? "n" : "",
                         x->value < 0 ? -x->value : x->value);
        else
            k = cx_fmt("%s@%s", k, x->tmpl ? x->tmpl->name : "?");
    }
    return k;
}

/* A value template parameter in an expression: T_, T0_ ... (no
 * substitution candidate there). */
static void put_param_expr(struct mbuf *m, struct cty *t)
{
    put(m, t->n == 0 ? "T_" : cx_fmt("T%ld_", t->n - 1));
}

/* Dp <type>: a pack expansion `T...` of a type (a substitution
 * candidate as a whole). */
static void mangle_dp(struct mbuf *m, struct cty *t)
{
    const char *key = cx_fmt("Dp%s", type_key(t));
    int i = sub_find(m, key);
    if (i >= 0) {
        put_sub(m, i);
        return;
    }
    put(m, "Dp");
    mangle_type(m, t);
    sub_add(m, key);
}

static void put_targ(struct mbuf *m, struct ctarg *x)
{
    if (x->expansion) {
        struct ctarg *p = &x->elems[0];
        if (p->kind == TP_VALUE) {
            put(m, "Xsp");                  /* Is... : an expression */
            if (p->mexpr)
                put_mexpr(m, p->mexpr);
            else
                put_param_expr(m, p->vtype);
            put(m, "E");
        } else {
            mangle_dp(m, p->type);          /* Ts... */
        }
        return;
    }
    if (x->is_pack) {
        put(m, "J");
        for (int i = 0; i < x->nelems; i++)
            put_targ(m, &x->elems[i]);
        put(m, "E");
        return;
    }
    if (x->kind == TP_TYPE) {
        mangle_type(m, x->type);
        return;
    }
    if (x->kind == TP_TEMPLATE) {
        put_template_name(m, x->tmpl);
        return;
    }
    if (x->mexpr) {
        put(m, "X");
        put_mexpr(m, x->mexpr);
        put(m, "E");
        return;
    }
    if (x->vtype && x->vtype->k == CT_TPARAM) {
        put(m, "X");                        /* the parameter, an expression */
        put_param_expr(m, x->vtype);
        put(m, "E");
        return;
    }
    if (x->vtype && x->vtype->k == CT_ENUM) {
        put(m, "L");
        mangle_type(m, x->vtype);
        put(m, cx_fmt("%s%ldE", x->value < 0 ? "n" : "",
                      x->value < 0 ? -x->value : x->value));
        return;
    }
    put(m, cx_fmt("L%s%s%ldE", value_code(x->vtype), x->value < 0 ? "n" : "",
                  x->value < 0 ? -x->value : x->value));
}

static void put_targs(struct mbuf *m, struct ctarg *a, int n)
{
    put(m, "I");
    for (int i = 0; i < n; i++)
        put_targ(m, &a[i]);
    put(m, "E");
}

static const char *operator_code(const char *name)
{
    static const struct { const char *op, *code; } ops[] = {
        { "operator new", "nw" }, { "operator new[]", "na" },
        { "operator delete", "dl" }, { "operator delete[]", "da" },
        { "operator+", "pl" }, { "operator-", "mi" }, { "operator*", "ml" },
        { "operator/", "dv" }, { "operator%", "rm" }, { "operator&", "an" },
        { "operator|", "or" }, { "operator^", "eo" }, { "operator=", "aS" },
        { "operator+=", "pL" }, { "operator-=", "mI" }, { "operator*=", "mL" },
        { "operator/=", "dV" }, { "operator%=", "rM" }, { "operator&=", "aN" },
        { "operator|=", "oR" }, { "operator^=", "eO" }, { "operator<<", "ls" },
        { "operator>>", "rs" }, { "operator<<=", "lS" }, { "operator>>=", "rS" },
        { "operator==", "eq" }, { "operator!=", "ne" }, { "operator<", "lt" },
        { "operator>", "gt" }, { "operator<=", "le" }, { "operator>=", "ge" },
        { "operator<=>", "ss" }, { "operator!", "nt" }, { "operator&&", "aa" },
        { "operator||", "oo" }, { "operator++", "pp" }, { "operator--", "mm" },
        { "operator,", "cm" }, { "operator->*", "pm" }, { "operator->", "pt" },
        { "operator()", "cl" }, { "operator[]", "ix" }, { "operator~", "co" },
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (strcmp(ops[i].op, name) == 0)
            return ops[i].code;
    return NULL;
}

/* The function's final (unqualified) name. */
static void put_unqualified(struct mbuf *m, struct cfunc *f)
{
    if (f->is_ctor) {
        put(m, f->ctor_variant == 2 ? "C2" : "C1");
        return;
    }
    if (f->is_dtor) {
        put(m, f->ctor_variant == 2 ? "D2" : f->ctor_variant == 0 ? "D0"
                                                                 : "D1");
        return;
    }
    if (f->is_conv) {
        put(m, "cv");               /* operator T: cv <type> */
        mangle_type(m, f->type->to);
        return;
    }
    const char *oc = operator_code(f->name);
    if (oc) {
        put(m, oc);
        return;
    }
    if (strncmp(f->name, "operator\"\"", 10) == 0) {
        put(m, "li");               /* a literal operator: li <suffix> */
        put_source(m, f->name + 10);
        return;
    }
    put_source(m, f->name);
}

const char *mangle_func(struct cfunc *f)
{
    if (f->c_linkage)
        return f->name;
    if (strcmp(f->name, "main") == 0 && f->owner == cx_global && !f->cls)
        return "main";
    struct mbuf m;
    memset(&m, 0, sizeof m);
    put(&m, "_Z");
    struct step st[128];
    int std;
    int n = scope_steps(f->owner, st, &std);
    int nested = n > 0 && !(n == 0 && std);
    const char *prev = n ? st[n - 1].key : std ? "St" : "";
    /* the final name: a source name, or an operator/special name, which
     * is never a candidate itself; a template's name is (its prefix) */
    struct mbuf u;
    memset(&u, 0, sizeof u);
    put_unqualified(&u, f);
    int tmpl = f->spec_of != NULL;
    /* steps: the scopes, then the name as written (a raw step), then the
     * template arguments */
    st[n].key = cx_fmt("%s%s", prev, u.p);
    st[n].name = NULL;
    st[n].args = NULL;
    st[n].nargs = 0;
    int name_step = n++;
    if (tmpl) {
        st[n].key = cx_fmt("%sI%sE", st[name_step].key,
                           targs_key(f->targs, f->ntargs));
        st[n].name = NULL;
        st[n].args = f->targs;
        st[n].nargs = f->ntargs;
        n++;
    }
    /* writing: the same as put_name, but the name step is raw text and
     * only a template's name (not a plain function's) is a candidate */
    int from = 0;
    st[name_step].abbr = NULL;
    if (tmpl)
        st[n - 1].abbr = NULL;
    for (int j2 = n - 1 - (tmpl ? 1 : 1); j2 >= 0; j2--) {
        if ((j2 == name_step && !tmpl) || st[j2].abbr)
            continue;
        int i2 = sub_find(&m, st[j2].key);
        if (i2 >= 0) {
            from = j2 + 1;
            break;
        }
    }
    nested = name_step > 0;
    if (nested || (std && name_step > 0)) {
        put(&m, "N");
        if (f->type->fq & CQ_VOLATILE) put(&m, "V");
        if (f->type->fq & CQ_CONST) put(&m, "K");
        if (f->type->refq) put(&m, f->type->refq == 1 ? "R" : "O");
    }
    if (std)
        put(&m, "St");
    if (from > 0)
        put_sub(&m, sub_find(&m, st[from - 1].key));
    for (int j2 = from; j2 < n; j2++) {
        if (j2 == name_step)
            put(&m, u.p);
        else if (st[j2].abbr) {
            put(&m, st[j2].abbr);        /* Sa, So...: no candidate */
            continue;
        } else if (st[j2].name)
            put_source(&m, st[j2].name);
        else
            put_targs(&m, st[j2].args, st[j2].nargs);
        if (j2 < name_step || (j2 == name_step && tmpl))
            sub_add(&m, st[j2].key);
    }
    if (nested || (std && name_step > 0))
        put(&m, "E");
    /* the signature: a specialization's as its template declares it,
     * return type first (in terms of T_, T0_ ...) */
    struct cty *ft = tmpl ? f->spec_of->pattern->type : f->type;
    if (tmpl && !f->is_ctor && !f->is_dtor && !f->is_conv)
        mangle_type(&m, ft->to);
    if (ft->np == 0 && !ft->variadic)
        put(&m, "v");
    for (int i = 0; i < ft->np; i++) {
        struct cty *pt = ft->params[i];
        if (pt->pack_expansion) {
            struct cty *e = xmalloc(sizeof *e);
            *e = *pt;
            e->pack_expansion = 0;
            mangle_dp(&m, e);               /* Ts... */
            continue;
        }
        mangle_type(&m, pt);
    }
    if (ft->variadic)
        put(&m, "z");
    return m.p;
}

const char *mangle_var(struct cvar *v, struct cscope *owner)
{
    if (v->c_linkage)
        return v->name;
    struct step st[128];
    int std;
    int n = scope_steps(owner, st, &std);
    if (n == 0 && !std && !v->targs)
        return v->name;          /* a global-namespace variable: unmangled */
    struct mbuf m;
    memset(&m, 0, sizeof m);
    put(&m, "_Z");
    const char *prev = n ? st[n - 1].key : std ? "St" : "";
    n = add_step(st, n, prev, v->name, v->targs, v->ntargs, v->targs != NULL);
    int last_name = n - 1;
    while (last_name > 0 && !st[last_name].name)
        last_name--;
    if (last_name > 0)
        put(&m, "N");
    if (std)
        put(&m, "St");
    /* the variable's own name is no candidate; its template's name is */
    put_steps(&m, st, n, v->targs ? 1 : 1);
    if (last_name > 0)
        put(&m, "E");
    return m.p;
}

/* A function's local static: _ZZ <the function's encoding> E <name>, with
 * _0, _1 ... telling apart same-named ones (disc counts from 0). */
const char *mangle_local_static(struct cvar *v, struct cfunc *fn, int disc)
{
    const char *enc = mangle_func(fn);
    if (strncmp(enc, "_Z", 2) == 0)
        enc += 2;
    else
        enc = cx_fmt("%zu%s", strlen(enc), enc);   /* extern "C" / main */
    if (disc == 0)
        return cx_fmt("_ZZ%sE%zu%s", enc, strlen(v->name), v->name);
    if (disc - 1 < 10)
        return cx_fmt("_ZZ%sE%zu%s_%d", enc, strlen(v->name), v->name,
                      disc - 1);
    return cx_fmt("_ZZ%sE%zu%s__%d_", enc, strlen(v->name), v->name,
                  disc - 1);
}

/* A type on its own (a typeinfo symbol's tail: _ZTI + this). */
const char *mangle_type_alone(struct cty *t)
{
    struct mbuf m;
    memset(&m, 0, sizeof m);
    mangle_type(&m, t);
    return m.p;
}

const char *mangle_class_name(struct cclass *c)
{
    struct mbuf m;
    memset(&m, 0, sizeof m);
    put_class_name(&m, c);
    return m.p;
}
