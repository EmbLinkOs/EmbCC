/* C++ types: construction, identity, sizes on the target, and the
 * conversions C++ shares with C (promotion, the usual arithmetic
 * conversions). Sizes are the LP64 ones both targets use; what differs by
 * target is only signedness (plain char and wchar_t are unsigned on aarch64)
 * and long double's format, which is emit.c's business. */
#include "cxx.h"

#include <stdio.h>
#include <string.h>

#include "../arch/target.h"
#include "../driver/util.h"

static struct cty basics[CT_AUTO + 1];

struct cty *ct_basic(enum cty_kind k)
{
    basics[k].k = k;
    basics[k].q = 0;
    return &basics[k];
}

static struct cty *ct_copy(const struct cty *t)
{
    struct cty *n = xmalloc(sizeof *n);
    *n = *t;
    return n;
}

struct cty *ct_qual(struct cty *t, unsigned q)
{
    if ((t->q | q) == t->q)
        return t;
    if (t->k == CT_ARRAY) {     /* cv on an array applies to its elements */
        struct cty *n = ct_copy(t);
        n->to = ct_qual(t->to, q);
        return n;
    }
    struct cty *n = ct_copy(t);
    n->q |= q;
    return n;
}

struct cty *ct_unqual(struct cty *t)
{
    if (!t->q)
        return t;
    if (t->k <= CT_VALIST || t->k == CT_AUTO)
        return ct_basic(t->k);
    struct cty *n = ct_copy(t);
    n->q = 0;
    return n;
}

struct cty *ct_ptr(struct cty *to)
{
    struct cty *n = xcalloc(1, sizeof *n);
    n->k = CT_PTR;
    n->to = to;
    return n;
}

struct cty *ct_ref(struct cty *to, int rvalue)
{
    /* reference collapsing: T& & -> T&, T&& & -> T&, T&& && -> T&& */
    if (to->k == CT_LREF || to->k == CT_RREF) {
        if (to->k == CT_LREF || !rvalue)
            return ct_ref(to->to, 0);
        return to;
    }
    struct cty *n = xcalloc(1, sizeof *n);
    n->k = rvalue ? CT_RREF : CT_LREF;
    n->to = to;
    return n;
}

struct cty *ct_array(struct cty *elem, long n)
{
    struct cty *t = xcalloc(1, sizeof *t);
    t->k = CT_ARRAY;
    t->to = elem;
    t->n = n;
    return t;
}

struct cty *ct_func(struct cty *ret, struct cty **params, int np, int variadic)
{
    struct cty *t = xcalloc(1, sizeof *t);
    t->k = CT_FUNC;
    t->to = ret;
    t->np = np;
    t->params = xmalloc((size_t)(np ? np : 1) * sizeof *t->params);
    for (int i = 0; i < np; i++)
        t->params[i] = params[i];
    t->variadic = variadic;
    return t;
}

struct cty *ct_class(struct cclass *c)
{
    struct cty *t = xcalloc(1, sizeof *t);
    t->k = CT_CLASS;
    t->cls = c;
    return t;
}

struct cty *ct_enum(struct cenum *e)
{
    struct cty *t = xcalloc(1, sizeof *t);
    t->k = CT_ENUM;
    t->en = e;
    return t;
}

struct cty *ct_mptr(struct cclass *c, struct cty *member)
{
    struct cty *t = xcalloc(1, sizeof *t);
    t->k = CT_MPTR;
    t->cls = c;
    t->to = member;
    return t;
}

int ct_is_pmf(const struct cty *t)
{
    return t->k == CT_MPTR && t->to->k == CT_FUNC;
}

struct cty *ct_tparam(int index, const char *name)
{
    struct cty *t = xcalloc(1, sizeof *t);
    t->k = CT_TPARAM;
    t->n = index;
    t->tpname = name;
    return t;
}

int ct_dependent(const struct cty *t)
{
    if (!t)
        return 0;
    switch (t->k) {
    case CT_TPARAM: case CT_TID: case CT_DEP:
        return 1;
    case CT_PTR: case CT_LREF: case CT_RREF: case CT_ARRAY:
        return t->n == -2 || t->n == -3 || ct_dependent(t->to);
    case CT_MPTR:
        return t->mclass || ct_dependent(t->to);
    case CT_FUNC:
        if (ct_dependent(t->to))
            return 1;
        for (int i = 0; i < t->np; i++)
            if (ct_dependent(t->params[i]))
                return 1;
        return 0;
    default:
        return 0;
    }
}

struct cty *ct_size_t(void) { return ct_basic(CT_ULONG); }
struct cty *ct_ptrdiff_t(void) { return ct_basic(CT_LONG); }

struct cty *ct_strip_ref(struct cty *t)
{
    return ct_is_ref(t) ? t->to : t;
}

static int same(const struct cty *a, const struct cty *b, int quals)
{
    if (a == b)
        return 1;
    if (a->k != b->k || (quals && a->q != b->q))
        return 0;
    switch (a->k) {
    case CT_PTR: case CT_LREF: case CT_RREF:
        return same(a->to, b->to, 1);
    case CT_ARRAY:
        return a->n == b->n && same(a->to, b->to, 1);
    case CT_FUNC:
        if (a->np != b->np || a->variadic != b->variadic || a->fq != b->fq ||
            !same(a->to, b->to, 1))
            return 0;
        for (int i = 0; i < a->np; i++)   /* top-level cv is not part of it */
            if (!same(a->params[i], b->params[i], 0))
                return 0;
        return 1;
    case CT_CLASS:
        return a->cls == b->cls;
    case CT_ENUM:
        return a->en == b->en;
    case CT_MPTR:
        if (a->mclass || b->mclass)
            return a->mclass && b->mclass && same(a->mclass, b->mclass, 1) &&
                   same(a->to, b->to, 1);
        return a->cls == b->cls && same(a->to, b->to, 1);
    case CT_TPARAM:
        return a->n == b->n;
    case CT_TID:
        if (a->tmpl != b->tmpl || a->ntargs != b->ntargs)
            return 0;
        for (int i = 0; i < a->ntargs; i++)
            if (!targ_same(&a->targs[i], &b->targs[i]))
                return 0;
        return 1;
    case CT_DEP:
        return 0;                 /* two unknown types: never known alike */
    case CT_COMPLEX:
        return a->to->k == b->to->k;
    default:
        return 1;
    }
}

/* Two template arguments are the same argument. */
int targ_same(const struct ctarg *a, const struct ctarg *b)
{
    if (a->kind != b->kind || a->is_pack != b->is_pack)
        return 0;
    if (a->is_pack) {
        if (a->nelems != b->nelems)
            return 0;
        for (int i = 0; i < a->nelems; i++)
            if (!targ_same(&a->elems[i], &b->elems[i]))
                return 0;
        return 1;
    }
    switch (a->kind) {
    case TP_TYPE: return ct_same(a->type, b->type);
    case TP_VALUE:
        if (a->mexpr || b->mexpr)
            return a->mexpr && b->mexpr &&
                   strcmp(mexpr_key(a->mexpr), mexpr_key(b->mexpr)) == 0;
        return a->value == b->value;
    default: return a->tmpl == b->tmpl;
    }
}

int ct_same(const struct cty *a, const struct cty *b) { return same(a, b, 1); }
int ct_same_unqual(const struct cty *a, const struct cty *b)
{
    return same(a, b, 0);
}

int ct_is_integer(const struct cty *t)
{
    return (t->k >= CT_BOOL && t->k <= CT_ULLONG) || t->k == CT_ENUM;
}

int ct_is_float(const struct cty *t)
{
    return t->k == CT_FLOAT || t->k == CT_DOUBLE || t->k == CT_LDOUBLE;
}

int ct_is_arith(const struct cty *t)
{
    return ct_is_integer(t) || ct_is_float(t);
}

int ct_is_scalar(const struct cty *t)
{
    return ct_is_arith(t) || t->k == CT_PTR || t->k == CT_NULLPTR ||
           t->k == CT_MPTR;
}

int ct_is_ref(const struct cty *t)
{
    return t->k == CT_LREF || t->k == CT_RREF;
}

int ct_is_void(const struct cty *t)
{
    return t->k == CT_VOID;
}

int ct_is_complete(const struct cty *t)
{
    switch (t->k) {
    case CT_VOID: return 0;
    case CT_ARRAY: return (t->n >= 0 || t->vla) && ct_is_complete(t->to);
    case CT_CLASS:
        class_ensure(t->cls);
        return t->cls->complete;
    case CT_ENUM: return t->en->complete || t->en->fixed;
    default: return 1;
    }
}

int ct_is_signed(const struct cty *t)
{
    switch (t->k) {
    case CT_CHAR:  return target_get() != TARGET_AARCH64;
    case CT_WCHAR: return target_get() != TARGET_AARCH64;
    case CT_SCHAR: case CT_SHORT: case CT_INT: case CT_LONG: case CT_LLONG:
        return 1;
    case CT_ENUM:  return ct_is_signed(t->en->underlying);
    default:
        return ct_is_float(t);
    }
}

struct cty *ct_complex(struct cty *elem)
{
    static struct cty *made[3];
    int i = elem->k == CT_FLOAT ? 0 : elem->k == CT_LDOUBLE ? 2 : 1;
    if (!made[i]) {
        made[i] = xcalloc(1, sizeof *made[i]);
        made[i]->k = CT_COMPLEX;
        made[i]->to = ct_basic(i == 0 ? CT_FLOAT : i == 2 ? CT_LDOUBLE
                                                         : CT_DOUBLE);
    }
    return made[i];
}

long ct_size(const struct cty *t)
{
    if (t->k == CT_CLASS)
        class_ensure(t->cls);
    switch (t->k) {
    case CT_VOID: return 1;          /* GNU: sizeof(void) for arithmetic */
    case CT_BOOL: case CT_CHAR: case CT_SCHAR: case CT_UCHAR: case CT_CHAR8:
        return 1;
    case CT_SHORT: case CT_USHORT: case CT_CHAR16: return 2;
    case CT_INT: case CT_UINT: case CT_WCHAR: case CT_CHAR32: case CT_FLOAT:
        return 4;
    case CT_LONG: case CT_ULONG: case CT_LLONG: case CT_ULLONG:
    case CT_DOUBLE: case CT_PTR: case CT_NULLPTR: case CT_LREF: case CT_RREF:
    case CT_VALIST:
        return 8;
    case CT_LDOUBLE: return 16;
    case CT_COMPLEX: return 2 * ct_size(t->to);
    case CT_ARRAY: return t->n < 0 ? 0 : t->n * ct_size(t->to);
    case CT_CLASS: return t->cls->size;
    case CT_ENUM: return ct_size(t->en->underlying);
    case CT_FUNC: return 1;
    case CT_MPTR: return t->to->k == CT_FUNC ? 16 : 8;
    case CT_AUTO: case CT_TPARAM: case CT_TID: case CT_DEP: return 0;
    }
    return 0;
}

long ct_align(const struct cty *t)
{
    if (t->k == CT_CLASS)
        class_ensure(t->cls);
    switch (t->k) {
    case CT_ARRAY: case CT_COMPLEX: return ct_align(t->to);
    case CT_CLASS: return t->cls->align;
    case CT_ENUM: return ct_align(t->en->underlying);
    case CT_FUNC: case CT_VOID: return 1;
    case CT_MPTR: return 8;
    default: return ct_size(t);
    }
}

/* Integral promotion (7.6): narrower than int -> int; the character types
 * whose values int cannot all hold -> unsigned int. */
struct cty *ct_promote(struct cty *t)
{
    t = ct_unqual(t);
    switch (t->k) {
    case CT_BOOL: case CT_CHAR: case CT_SCHAR: case CT_UCHAR: case CT_CHAR8:
    case CT_SHORT: case CT_USHORT: case CT_CHAR16:
        return ct_basic(CT_INT);
    case CT_WCHAR:
        return ct_basic(ct_is_signed(t) ? CT_INT : CT_UINT);
    case CT_CHAR32:
        return ct_basic(CT_UINT);
    case CT_ENUM:
        return ct_promote(t->en->underlying);
    default:
        return t;
    }
}

static int rank(enum cty_kind k)
{
    switch (k) {
    case CT_INT: case CT_UINT: return 1;
    case CT_LONG: case CT_ULONG: return 2;
    case CT_LLONG: case CT_ULLONG: return 3;
    default: return 0;
    }
}

static enum cty_kind to_unsigned(enum cty_kind k)
{
    switch (k) {
    case CT_INT: return CT_UINT;
    case CT_LONG: return CT_ULONG;
    case CT_LLONG: return CT_ULLONG;
    default: return k;
    }
}

/* The usual arithmetic conversions (7.4). */
struct cty *ct_arith_common(struct cty *a, struct cty *b)
{
    a = ct_unqual(a);
    b = ct_unqual(b);
    if (a->k == CT_LDOUBLE || b->k == CT_LDOUBLE) return ct_basic(CT_LDOUBLE);
    if (a->k == CT_DOUBLE || b->k == CT_DOUBLE) return ct_basic(CT_DOUBLE);
    if (a->k == CT_FLOAT || b->k == CT_FLOAT) return ct_basic(CT_FLOAT);
    a = ct_promote(a);
    b = ct_promote(b);
    if (a->k == b->k)
        return a;
    int sa = ct_is_signed(a), sb = ct_is_signed(b);
    if (sa == sb)
        return rank(a->k) >= rank(b->k) ? a : b;
    struct cty *u = sa ? b : a, *s = sa ? a : b;
    if (rank(u->k) >= rank(s->k))
        return u;
    if (ct_size(s) > ct_size(u))
        return s;
    return ct_basic(to_unsigned(s->k));
}

struct cty *ct_decay(struct cty *t)
{
    if (t->k == CT_ARRAY)
        return ct_ptr(t->to);
    if (t->k == CT_FUNC)
        return ct_ptr(t);
    return t;
}

static const char *basic_name(enum cty_kind k)
{
    switch (k) {
    case CT_VOID: return "void";
    case CT_BOOL: return "bool";
    case CT_CHAR: return "char";
    case CT_SCHAR: return "signed char";
    case CT_UCHAR: return "unsigned char";
    case CT_WCHAR: return "wchar_t";
    case CT_CHAR8: return "char8_t";
    case CT_CHAR16: return "char16_t";
    case CT_CHAR32: return "char32_t";
    case CT_SHORT: return "short";
    case CT_USHORT: return "unsigned short";
    case CT_INT: return "int";
    case CT_UINT: return "unsigned int";
    case CT_LONG: return "long";
    case CT_ULONG: return "unsigned long";
    case CT_LLONG: return "long long";
    case CT_ULLONG: return "unsigned long long";
    case CT_FLOAT: return "float";
    case CT_DOUBLE: return "double";
    case CT_LDOUBLE: return "long double";
    case CT_NULLPTR: return "std::nullptr_t";
    case CT_VALIST: return "__builtin_va_list";
    case CT_AUTO: return "auto";
    default: return "?";
    }
}

static void name_into(char *buf, size_t cap, const struct cty *t)
{
    char inner[256];
    const char *cv = (t->q & CQ_CONST) ? ((t->q & CQ_VOLATILE)
                     ? "const volatile " : "const ")
                   : (t->q & CQ_VOLATILE) ? "volatile " : "";
    switch (t->k) {
    case CT_COMPLEX:
        name_into(inner, sizeof inner, t->to);
        snprintf(buf, cap, "%s__complex__ %s", cv, inner);
        return;
    case CT_PTR: case CT_LREF: case CT_RREF:
        name_into(inner, sizeof inner, t->to);
        snprintf(buf, cap, "%s%s%s", inner,
                 t->k == CT_PTR ? "*" : t->k == CT_LREF ? "&" : "&&",
                 (t->q & CQ_CONST) ? " const" : "");
        return;
    case CT_ARRAY:
        name_into(inner, sizeof inner, t->to);
        if (t->n >= 0) snprintf(buf, cap, "%s[%ld]", inner, t->n);
        else snprintf(buf, cap, "%s[]", inner);
        return;
    case CT_FUNC: {
        name_into(inner, sizeof inner, t->to);
        size_t n = (size_t)snprintf(buf, cap, "%s(", inner);
        for (int i = 0; i < t->np && n < cap; i++) {
            char p[128];
            name_into(p, sizeof p, t->params[i]);
            n += (size_t)snprintf(buf + n, cap - n, "%s%s", i ? ", " : "", p);
        }
        if (n < cap)
            snprintf(buf + n, cap - n, "%s)", t->variadic ? ", ..." : "");
        return;
    }
    case CT_CLASS:
        snprintf(buf, cap, "%s%s", cv, t->cls->name ? t->cls->name
                                                   : "<anonymous>");
        return;
    case CT_MPTR: {
        char cn[256];
        name_into(inner, sizeof inner, t->to);
        if (t->mclass)
            name_into(cn, sizeof cn, t->mclass);
        snprintf(buf, cap, "%s %s::*", inner, t->mclass ? cn
                                              : t->cls->name ? t->cls->name
                                              : "<anonymous>");
        return;
    }
    case CT_TPARAM:
        snprintf(buf, cap, "%s%s", cv, t->tpname ? t->tpname : "T");
        return;
    case CT_TID:
        snprintf(buf, cap, "%s%s<...>", cv, t->tmpl->name);
        return;
    case CT_DEP:
        snprintf(buf, cap, "%s<dependent type>", cv);
        return;
    case CT_ENUM:
        snprintf(buf, cap, "%s%s", cv, t->en->name ? t->en->name
                                                  : "<anonymous enum>");
        return;
    default:
        snprintf(buf, cap, "%s%s", cv, basic_name(t->k));
    }
}

const char *ct_name(const struct cty *t)
{
    static char bufs[4][256];
    static int which;
    char *b = bufs[which];
    which = (which + 1) & 3;
    name_into(b, 256, t);
    return b;
}

int is_std_il(const struct ctemplate *t)
{
    return t && t->kind == TK_CLASS && t->name &&
           strcmp(t->name, "initializer_list") == 0 && t->scope &&
           t->scope->k == SC_NAMESPACE && t->scope->name &&
           strcmp(t->scope->name, "std") == 0 &&
           t->scope->parent == cx_global;
}

struct cty *ct_il_elem(struct cty *t)
{
    if (!t)
        return NULL;
    if (ct_is_ref(t))
        t = t->to;
    if (t->k != CT_CLASS || !t->cls->tmpl || !is_std_il(t->cls->tmpl) ||
        t->cls->ntargs != 1 || t->cls->targs[0].kind != TP_TYPE)
        return NULL;
    return t->cls->targs[0].type;
}

int ct_il_param(struct cty *t)
{
    if (!t)
        return 0;
    if (ct_is_ref(t))
        t = t->to;
    if (t->k == CT_TID)
        return is_std_il(t->tmpl);
    return ct_il_elem(t) != NULL;
}

int ct_has_auto(const struct cty *t)
{
    while (t && (t->k == CT_LREF || t->k == CT_RREF || t->k == CT_PTR))
        t = t->to;
    return t && t->k == CT_AUTO;
}

/* Does t involve a class with no linkage — local, unnamed, or an
 * instance for such a class? Then so has what is made from it. */
int ct_is_local(const struct cty *t)
{
    while (t && (t->k == CT_PTR || t->k == CT_LREF || t->k == CT_RREF ||
                 t->k == CT_ARRAY || t->k == CT_MPTR)) {
        if (t->k == CT_MPTR && t->cls && (t->cls->local || t->cls->anon))
            return 1;
        t = t->to;
    }
    if (!t)
        return 0;
    if (t->k == CT_FUNC) {
        if (ct_is_local(t->to))
            return 1;
        for (int i = 0; i < t->np; i++)
            if (ct_is_local(t->params[i]))
                return 1;
        return 0;
    }
    return t->k == CT_CLASS && (t->cls->local || t->cls->anon);
}

int targs_local(const struct ctarg *a, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i].is_pack && targs_local(a[i].elems, a[i].nelems))
            return 1;
        if (a[i].kind == TP_TYPE && a[i].type && ct_is_local(a[i].type))
            return 1;
    }
    return 0;
}
