/* Itanium C++ ABI name mangling (both targets use it) — what makes EmbCC's
 * objects link with g++'s and with libstdc++. The part that is easy to get
 * wrong is substitution: every substitutable component (a name prefix, a
 * class or enum name, a qualified or compound type) is numbered in the order
 * it is mangled, and a repeat is written S_, S0_, S1_... instead. The
 * function's own final name is never a candidate; builtin types are not
 * either, though a cv-qualified one is. */
#include "cxx.h"

#include <stdio.h>
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

static void put_source(struct mbuf *m, const char *id)
{
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
    return s->name;
}

/* Writes the prefix components for the scopes, each cumulative prefix a
 * substitution candidate: the longest one already seen as an S-reference,
 * then the remaining components. std:: is `St`, not itself a candidate. */
static void put_prefix(struct mbuf *m, struct cscope **chain, int n)
{
    int i = 0;
    if (n > 0 && is_std(chain[0])) {
        put(m, "St");
        i = 1;                  /* St itself is not a candidate */
    }
    /* emit greedily: longest prefix already substituted, then the rest */
    int best = -1, bestj = i - 1;
    char *acc = xstrndup("", 0);
    size_t acclen = 0;
    char *prefixes[64];
    for (int j = i; j < n; j++) {
        char comp[256];
        snprintf(comp, sizeof comp, "%zu%s", strlen(scope_ident(chain[j])),
                 scope_ident(chain[j]));
        size_t cl = strlen(comp);
        char *na = xmalloc(acclen + cl + 1);
        memcpy(na, acc, acclen);
        memcpy(na + acclen, comp, cl + 1);
        acc = na;
        acclen += cl;
        prefixes[j] = acc;
        int s = sub_find(m, is_std(chain[0]) && i == 1
                            ? cx_fmt("St%s", acc) : acc);
        if (s >= 0) {
            best = s;
            bestj = j;
        }
    }
    if (best >= 0)
        put_sub(m, best);
    for (int j = bestj + 1; j < n; j++) {
        put_source(m, scope_ident(chain[j]));
        sub_add(m, is_std(chain[0]) && i == 1 ? cx_fmt("St%s", prefixes[j])
                                              : prefixes[j]);
    }
}

static void mangle_type(struct mbuf *m, struct cty *t);
static const char *type_name_key(struct cscope *owner, const char *name);

/* A class or enum name: `3Foo` at global scope, `St3Foo` in std, or
 * `N3foo3FooE` — or its substitution when it was seen already. */
static void put_type_name(struct mbuf *m, struct cscope *owner,
                          const char *name)
{
    struct cscope *chain[64];
    int n = scope_chain(owner, chain, 64);
    const char *key = type_name_key(owner, name);
    int i = sub_find(m, key);
    if (i >= 0) {
        put_sub(m, i);
        return;
    }
    if (n == 0) {
        put_source(m, name);
    } else if (n == 1 && is_std(chain[0])) {
        put(m, "St");               /* ::std::Foo is St3Foo, not nested */
        put_source(m, name);
    } else {
        put(m, "N");
        put_prefix(m, chain, n);
        put_source(m, name);
        put(m, "E");
    }
    sub_add(m, key);
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
    default: return NULL;
    }
}

/* A class or enum's key in the substitution table: its components, as the
 * prefixes naming its enclosing scopes are keyed (so the prefix `abi::X::`
 * of a nested name and the type abi::X are one candidate). */
static const char *type_name_key(struct cscope *owner, const char *name)
{
    struct cscope *chain[64];
    int n = scope_chain(owner, chain, 64);
    char full[1024];
    size_t fl = 0;
    int stdp = n > 0 && is_std(chain[0]);
    for (int j = stdp ? 1 : 0; j < n; j++)
        fl += (size_t)snprintf(full + fl, sizeof full - fl, "%zu%s",
                               strlen(scope_ident(chain[j])),
                               scope_ident(chain[j]));
    snprintf(full + fl, sizeof full - fl, "%zu%s", strlen(name), name);
    return stdp ? cx_fmt("St%s", full) : cx_strdup(full);
}

/* t's key in the substitution table: an encoding with no substitutions in
 * it, so a repeat is recognized however its first occurrence was written
 * (its own parts may have been abbreviated there). */
static const char *type_key(struct cty *t)
{
    if (t->q && t->k != CT_FUNC)
        return cx_fmt("%s%s%s", (t->q & CQ_VOLATILE) ? "V" : "",
                      (t->q & CQ_CONST) ? "K" : "", type_key(ct_unqual(t)));
    const char *b = builtin_code(t->k);
    if (b)
        return b;
    switch (t->k) {
    case CT_PTR: return cx_fmt("P%s", type_key(t->to));
    case CT_LREF: return cx_fmt("R%s", type_key(t->to));
    case CT_RREF: return cx_fmt("O%s", type_key(t->to));
    case CT_ARRAY:
        return t->n >= 0 ? cx_fmt("A%ld_%s", t->n, type_key(t->to))
                         : cx_fmt("A_%s", type_key(t->to));
    case CT_FUNC: {
        char *k = cx_fmt("F%s", type_key(t->to));
        for (int i = 0; i < t->np; i++)
            k = cx_fmt("%s%s", k, type_key(t->params[i]));
        return cx_fmt("%s%s%sE", k, t->np == 0 && !t->variadic ? "v" : "",
                      t->variadic ? "z" : "");
    }
    case CT_CLASS:
        return type_name_key(t->cls->owner,
                             t->cls->name ? t->cls->name : "._anon");
    case CT_ENUM:
        return type_name_key(t->en->owner,
                             t->en->name ? t->en->name : "._anon");
    default:
        return "?";
    }
}

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
    const char *b = builtin_code(t->k);
    if (b) {
        put(m, b);
        return;
    }
    switch (t->k) {
    case CT_PTR: case CT_LREF: case CT_RREF: case CT_ARRAY: case CT_FUNC: {
        const char *key = type_key(t);
        int i = sub_find(m, key);
        if (i >= 0) {
            put_sub(m, i);
            return;
        }
        /* written out: its parts are numbered first, then the whole */
        switch (t->k) {
        case CT_PTR: put(m, "P"); mangle_type(m, t->to); break;
        case CT_LREF: put(m, "R"); mangle_type(m, t->to); break;
        case CT_RREF: put(m, "O"); mangle_type(m, t->to); break;
        case CT_ARRAY: {
            char n[32];
            if (t->n >= 0) snprintf(n, sizeof n, "A%ld_", t->n);
            else snprintf(n, sizeof n, "A_");
            put(m, n);
            mangle_type(m, t->to);
            break;
        }
        default:
            put(m, "F");
            mangle_type(m, t->to);
            if (t->np == 0 && !t->variadic)
                put(m, "v");
            for (int i2 = 0; i2 < t->np; i2++)
                mangle_type(m, t->params[i2]);
            if (t->variadic)
                put(m, "z");
            put(m, "E");
        }
        sub_add(m, key);
        return;
    }
    case CT_CLASS:
        put_type_name(m, t->cls->owner,
                      t->cls->name ? t->cls->name : "._anon");
        return;
    case CT_ENUM:
        put_type_name(m, t->en->owner, t->en->name ? t->en->name : "._anon");
        return;
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
    const char *oc = operator_code(f->name);
    if (oc) {
        put(m, oc);
        return;
    }
    put_source(m, f->name);
}

const char *mangle_func(struct cfunc *f)
{
    if (f->c_linkage)
        return f->name;
    if (strcmp(f->name, "main") == 0 && f->owner == cx_global)
        return "main";
    struct mbuf m;
    memset(&m, 0, sizeof m);
    put(&m, "_Z");
    struct cscope *chain[64];
    int n = scope_chain(f->owner, chain, 64);
    if (n == 0) {
        put_unqualified(&m, f);
    } else if (n == 1 && is_std(chain[0]) && !f->cls) {
        put(&m, "St");
        put_unqualified(&m, f);
    } else {
        put(&m, "N");
        if (f->type->fq & CQ_VOLATILE) put(&m, "V");
        if (f->type->fq & CQ_CONST) put(&m, "K");
        put_prefix(&m, chain, n);
        put_unqualified(&m, f);
        put(&m, "E");
    }
    struct cty *ft = f->type;
    if (ft->np == 0 && !ft->variadic)
        put(&m, "v");
    for (int i = 0; i < ft->np; i++)
        mangle_type(&m, ft->params[i]);
    if (ft->variadic)
        put(&m, "z");
    return m.p;
}

const char *mangle_var(struct cvar *v, struct cscope *owner)
{
    if (v->c_linkage)
        return v->name;
    struct cscope *chain[64];
    int n = scope_chain(owner, chain, 64);
    if (n == 0)
        return v->name;          /* a global-namespace variable: unmangled */
    struct mbuf m;
    memset(&m, 0, sizeof m);
    put(&m, "_Z");
    if (n == 1 && is_std(chain[0])) {
        put(&m, "St");
        put_source(&m, v->name);
        return m.p;
    }
    put(&m, "N");
    put_prefix(&m, chain, n);
    put_source(&m, v->name);
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

const char *mangle_class_name(struct cclass *c)
{
    struct mbuf m;
    memset(&m, 0, sizeof m);
    put_type_name(&m, c->owner,
                  c->name ? c->name : "._anon");
    return m.p;
}
