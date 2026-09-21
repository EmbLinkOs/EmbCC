/* The type-trait intrinsics libstdc++'s <type_traits> is built on
 * (__is_class(T), __is_constructible(T, Args...), __underlying_type(T),
 * ...): g++'s names, answered from what the front-end knows of the types.
 *
 * The ones asking whether an expression would be well-formed
 * (constructible, assignable, convertible) build it — from operands as
 * std::declval<T>() gives them: T& an lvalue, anything else an xvalue —
 * with errors caught (cx_sfinae), as a substitution failure is. */
#include "cxx.h"

#include <setjmp.h>
#include <string.h>

#include "../driver/util.h"

enum {
    TR_BOOL1,   /* bool, one type */
    TR_BOOL2,   /* bool, two types */
    TR_BOOLN,   /* bool, a type then any number */
    TR_TYPE1    /* a type, from one type */
};

static const struct trait {
    const char *name;
    int kind;
} traits[] = {
    { "__is_class", TR_BOOL1 }, { "__is_union", TR_BOOL1 },
    { "__is_enum", TR_BOOL1 }, { "__is_empty", TR_BOOL1 },
    { "__is_polymorphic", TR_BOOL1 }, { "__is_abstract", TR_BOOL1 },
    { "__is_final", TR_BOOL1 }, { "__is_trivial", TR_BOOL1 },
    { "__is_trivially_copyable", TR_BOOL1 }, { "__is_pod", TR_BOOL1 },
    { "__is_standard_layout", TR_BOOL1 }, { "__is_literal_type", TR_BOOL1 },
    { "__has_trivial_destructor", TR_BOOL1 },
    { "__is_trivially_destructible", TR_BOOL1 },
    { "__has_virtual_destructor", TR_BOOL1 }, { "__is_aggregate", TR_BOOL1 },
    { "__has_unique_object_representations", TR_BOOL1 },
    { "__is_scoped_enum", TR_BOOL1 }, { "__is_function", TR_BOOL1 },
    { "__is_array", TR_BOOL1 }, { "__is_bounded_array", TR_BOOL1 },
    { "__is_unbounded_array", TR_BOOL1 }, { "__is_pointer", TR_BOOL1 },
    { "__is_reference", TR_BOOL1 }, { "__is_const", TR_BOOL1 },
    { "__is_volatile", TR_BOOL1 }, { "__is_object", TR_BOOL1 },
    { "__is_member_pointer", TR_BOOL1 },
    { "__is_member_function_pointer", TR_BOOL1 },
    { "__is_member_object_pointer", TR_BOOL1 },
    { "__is_destructible", TR_BOOL1 },
    { "__is_nothrow_destructible", TR_BOOL1 },
    { "__is_same", TR_BOOL2 }, { "__is_base_of", TR_BOOL2 },
    { "__is_convertible", TR_BOOL2 }, { "__is_nothrow_convertible", TR_BOOL2 },
    { "__is_assignable", TR_BOOL2 }, { "__is_trivially_assignable", TR_BOOL2 },
    { "__is_nothrow_assignable", TR_BOOL2 },
    { "__is_constructible", TR_BOOLN },
    { "__is_trivially_constructible", TR_BOOLN },
    { "__is_nothrow_constructible", TR_BOOLN },
    { "__underlying_type", TR_TYPE1 }, { "__remove_cv", TR_TYPE1 },
    { "__remove_reference", TR_TYPE1 }, { "__remove_cvref", TR_TYPE1 },
    { "__remove_pointer", TR_TYPE1 }, { "__add_pointer", TR_TYPE1 },
    { "__add_lvalue_reference", TR_TYPE1 },
    { "__add_rvalue_reference", TR_TYPE1 }, { "__decay", TR_TYPE1 },
    { "__remove_extent", TR_TYPE1 }, { "__remove_all_extents", TR_TYPE1 },
};

static const struct trait *find_trait(const char *name)
{
    for (size_t i = 0; i < sizeof traits / sizeof traits[0]; i++)
        if (strcmp(traits[i].name, name) == 0)
            return &traits[i];
    return NULL;
}

int trait_known(const char *name)
{
    return find_trait(name) != NULL;
}

int trait_is_type(const char *name)
{
    const struct trait *t = find_trait(name);
    return t && t->kind == TR_TYPE1;
}

int trait_at(void)
{
    if (cx_kind() != TOK_IDENT || cx_kind_at(1) != TOK_LPAREN)
        return 0;
    const char *n = cx_cur()->t.text;
    if (n[0] != '_' || n[1] != '_' || !find_trait(n))
        return 0;
    struct csym *y = lookup(cx_scope, n);
    return !y;                 /* a declared name of that spelling wins */
}

/* ( type, type..., Pack... ) — the types, expansions expanded */
static int trait_args(struct cty ***out)
{
    int n = 0, cap = 4;
    struct cty **v = xmalloc((size_t)cap * sizeof *v);
    cx_expect(TOK_LPAREN, "'('");
    int saved = cx_in_targs;
    cx_in_targs = 0;
    while (cx_kind() != TOK_RPAREN) {
        struct csym *packs[8];
        int ell;
        int ex = expansion_at(0, packs, 8, &ell);
        if (ex < 0)
            cx_error(&cx_toks[ell], "'...' expands no parameter pack");
        int reps = ex > 0 ? expansion_length(packs, ex) : 1;
        int start = cx_pos;
        for (int e = 0; e < reps; e++) {
            cx_pos = start;
            for (int k = 0; k < ex; k++)
                pack_push(packs[k], e);
            if (n == cap) {
                cap *= 2;
                v = xrealloc(v, (size_t)cap * sizeof *v);
            }
            v[n++] = parse_type_id();
            if (ex > 0)
                pack_pop(ex);
        }
        if (ex > 0) {
            cx_pos = ell;
            cx_expect(TOK_ELLIPSIS, "'...'");
        }
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_expect(TOK_RPAREN, "')' after a trait's types");
    cx_in_targs = saved;
    *out = v;
    return n;
}

/* ---- the answers ---- */

/* T without top-level cv — an array's is its elements' (const char[3]
 * is char[3]) */
static struct cty *remove_cv(struct cty *t)
{
    if (t->k == CT_ARRAY)
        return ct_array(remove_cv(t->to), t->n);
    return ct_unqual(t);
}

static struct cclass *class_of(struct cty *t)
{
    if (t->k != CT_CLASS)
        return NULL;
    class_ensure(t->cls);
    return t->cls;
}

static int is_scalar_ish(struct cty *t)
{
    return ct_is_scalar(t);
}

static int trivially_copyable(struct cty *t)
{
    while (t->k == CT_ARRAY)
        t = t->to;
    if (is_scalar_ish(t))
        return 1;
    struct cclass *c = class_of(t);
    return c && c->complete && c->trivial_copy && c->trivial_assign &&
           c->trivial_dtor && !c->dynamic && !c->nvbases;
}

static int trivial(struct cty *t)
{
    while (t->k == CT_ARRAY)
        t = t->to;
    if (is_scalar_ish(t))
        return 1;
    struct cclass *c = class_of(t);
    return c && trivially_copyable(t) && c->trivial_default;
}

static int standard_layout(struct cty *t)
{
    while (t->k == CT_ARRAY)
        t = t->to;
    if (is_scalar_ish(t))
        return 1;
    struct cclass *c = class_of(t);
    if (!c || c->dynamic || c->nvbases)
        return 0;
    /* data in at most one class of the hierarchy */
    int with = c->nfields > 0;
    for (int i = 0; i < c->nbases; i++) {
        struct cty *bt = ct_class(c->bases[i].cls);
        if (!standard_layout(bt))
            return 0;
        if (c->bases[i].cls->nfields)
            with++;
    }
    for (int i = 0; i < c->nfields; i++)
        if (!ct_is_ref(c->fields[i]->type) &&
            !standard_layout(c->fields[i]->type))
            return 0;
    return with <= 1;
}

static int trivially_destructible(struct cty *t)
{
    while (t->k == CT_ARRAY)
        t = t->to;
    if (ct_is_ref(t) || is_scalar_ish(t))
        return 1;
    struct cclass *c = class_of(t);
    return c && c->complete && c->trivial_dtor;
}

/* padding-free: every bit of the value takes part in it */
static int unique_repr(struct cty *t)
{
    while (t->k == CT_ARRAY)
        t = t->to;
    if (ct_is_integer(t) || t->k == CT_PTR || t->k == CT_MPTR)
        return t->k != CT_MPTR || !ct_is_pmf(t);
    if (ct_is_float(t))
        return 0;
    struct cclass *c = class_of(t);
    if (!c || !trivially_copyable(t) || c->is_union)
        return 0;
    long sum = 0;
    for (int i = 0; i < c->nbases; i++) {
        if (!unique_repr(ct_class(c->bases[i].cls)))
            return 0;
        sum += c->bases[i].cls->empty ? 0 : ct_size(ct_class(c->bases[i].cls));
    }
    for (int i = 0; i < c->nfields; i++) {
        struct cfield *f = c->fields[i];
        if (f->bitwidth >= 0 || !unique_repr(f->type))
            return 0;
        sum += ct_size(f->type);
    }
    return sum == c->size;
}

static int is_object(struct cty *t)
{
    return t->k != CT_FUNC && !ct_is_ref(t) && t->k != CT_VOID;
}

/* std::declval<T>()'s expression */
static struct cexpr *declval(struct cty *t)
{
    struct cvar *v = xcalloc(1, sizeof *v);
    v->name = v->cname = "__cx_declval";
    v->type = t;
    int vc = t->k == CT_LREF ? VC_LVALUE
             : t->k == CT_FUNC ? VC_LVALUE : VC_XVALUE;
    struct cexpr *e = ex_new(E_VAR, ct_strip_ref(t), vc);
    e->var = v;
    return e;
}

/* The expressions a trait tries: built with errors turned into failure
 * (a substitution failure), the parser's state kept. */
enum { TRY_CONSTRUCT, TRY_ASSIGN, TRY_CONVERT, TRY_DESTROY };

static struct cexpr *try_build(int what, struct cty *t, struct cty **args,
                               int na, int *ok)
{
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    struct cexpr *volatile r = NULL;
    *ok = 0;
    if (setjmp(jb)) {
        parse_restore(st);
        cx_sfinae = saved;
        return NULL;
    }
    cx_sfinae = &jb;
    const struct ctok *at = cx_cur();
    switch (what) {
    case TRY_CONSTRUCT: {
        struct cexpr **a = xcalloc((size_t)(na ? na : 1), sizeof *a);
        for (int i = 0; i < na; i++)
            a[i] = declval(args[i]);
        if (ct_is_ref(t)) {
            if (na != 1)
                cx_error(at, "not constructible");
            r = bind_ref(a[0], t, "a trait");
        } else if (na == 0) {
            r = init_object(t, INIT_VALUE, NULL, 0, at);
            if (!r)
                r = ex_int(0, ct_basic(CT_INT));
        } else {
            r = init_object(t, INIT_DIRECT, a, na, at);
        }
        break;
    }
    case TRY_ASSIGN:
        r = expr_assign(TOK_ASSIGN, declval(t), declval(args[0]));
        break;
    case TRY_CONVERT: {
        /* To test() { return declval<From>(); } */
        struct cty *to = args[0];
        struct cexpr *e = declval(t);
        if (to->k == CT_VOID) {
            r = e;
        } else if (ct_is_ref(to)) {
            r = bind_ref(e, to, "a trait");
        } else {
            if (!is_object(to) || to->k == CT_ARRAY)
                cx_error(at, "not convertible");
            r = init_object(ct_unqual(to), INIT_COPY, &e, 1, at);
        }
        break;
    }
    case TRY_DESTROY: {
        struct cty *u = t;
        while (u->k == CT_ARRAY)
            u = u->to;
        struct cclass *c = class_of(u);
        if (c) {
            struct cfunc *d = class_dtor(c);
            if (d && d->is_deleted)
                cx_error(at, "not destructible");
            r = d ? ex_new(E_CALL, ct_basic(CT_VOID), VC_PRVALUE) : NULL;
            if (r)
                r->fn = d;
        }
        break;
    }
    }
    cx_sfinae = saved;
    parse_restore(st);
    *ok = 1;
    return r;
}

static long bool_trait(const char *n, struct cty **a, int na)
{
    struct cty *t = a[0];
    struct cclass *c = t->k == CT_CLASS ? class_of(t) : NULL;
    if (!strcmp(n, "__is_class"))
        return c && !c->is_union;
    if (!strcmp(n, "__is_union"))
        return c && c->is_union;
    if (!strcmp(n, "__is_enum"))
        return t->k == CT_ENUM;
    if (!strcmp(n, "__is_scoped_enum"))
        return t->k == CT_ENUM && t->en->scoped;
    if (!strcmp(n, "__is_empty"))
        return c && !c->is_union && c->empty;
    if (!strcmp(n, "__is_polymorphic"))
        return c && c->dynamic;
    if (!strcmp(n, "__is_abstract"))
        return c && c->dynamic && class_abstract(c);
    if (!strcmp(n, "__is_final"))
        return c && c->is_final;
    if (!strcmp(n, "__is_trivial"))
        return trivial(t);
    if (!strcmp(n, "__is_trivially_copyable"))
        return trivially_copyable(t);
    if (!strcmp(n, "__is_standard_layout"))
        return standard_layout(t);
    if (!strcmp(n, "__is_pod"))
        return trivial(t) && standard_layout(t);
    if (!strcmp(n, "__is_literal_type"))
        return t->k == CT_VOID || ct_is_ref(t) || is_scalar_ish(t) ||
               trivially_destructible(t);
    if (!strcmp(n, "__has_trivial_destructor") ||
        !strcmp(n, "__is_trivially_destructible"))
        return trivially_destructible(t);
    if (!strcmp(n, "__has_virtual_destructor")) {
        struct cfunc *d = c ? class_dtor(c) : NULL;
        return d && d->is_virtual;
    }
    if (!strcmp(n, "__is_aggregate"))
        return t->k == CT_ARRAY || (c && c->aggregate);
    if (!strcmp(n, "__has_unique_object_representations"))
        return unique_repr(t);
    if (!strcmp(n, "__is_function"))
        return t->k == CT_FUNC;
    if (!strcmp(n, "__is_array"))
        return t->k == CT_ARRAY;
    if (!strcmp(n, "__is_bounded_array"))
        return t->k == CT_ARRAY && t->n >= 0;
    if (!strcmp(n, "__is_unbounded_array"))
        return t->k == CT_ARRAY && t->n < 0;
    if (!strcmp(n, "__is_pointer"))
        return t->k == CT_PTR;
    if (!strcmp(n, "__is_reference"))
        return ct_is_ref(t);
    if (!strcmp(n, "__is_const"))
        return (t->q & CQ_CONST) != 0;
    if (!strcmp(n, "__is_volatile"))
        return (t->q & CQ_VOLATILE) != 0;
    if (!strcmp(n, "__is_object"))
        return is_object(t);
    if (!strcmp(n, "__is_member_pointer"))
        return t->k == CT_MPTR;
    if (!strcmp(n, "__is_member_function_pointer"))
        return t->k == CT_MPTR && ct_is_pmf(t);
    if (!strcmp(n, "__is_member_object_pointer"))
        return t->k == CT_MPTR && !ct_is_pmf(t);
    if (!strcmp(n, "__is_destructible") ||
        !strcmp(n, "__is_nothrow_destructible")) {
        if (ct_is_ref(t))
            return 1;
        if (t->k == CT_VOID || t->k == CT_FUNC ||
            (t->k == CT_ARRAY && t->n < 0))
            return 0;
        int ok;
        struct cexpr *d = try_build(TRY_DESTROY, t, NULL, 0, &ok);
        if (!ok)
            return 0;
        if (n[5] == 'n')        /* nothrow: destructors are, unless said */
            return !d || !d->fn || func_nothrow(d->fn);
        return 1;
    }
    if (!strcmp(n, "__is_same"))
        return na == 2 && ct_same(t, a[1]);
    if (!strcmp(n, "__is_base_of")) {
        struct cty *d = a[1];
        if (t->k != CT_CLASS || d->k != CT_CLASS)
            return 0;
        class_ensure(d->cls);
        return t->cls == d->cls || class_derives(d->cls, t->cls, NULL);
    }
    if (!strcmp(n, "__is_convertible") ||
        !strcmp(n, "__is_nothrow_convertible")) {
        struct cty *to = a[1];
        if (to->k == CT_VOID || t->k == CT_VOID)
            return to->k == CT_VOID && t->k == CT_VOID ? 1
                   : to->k == CT_VOID;
        if (to->k == CT_FUNC || to->k == CT_ARRAY)
            return 0;
        int ok;
        struct cexpr *e = try_build(TRY_CONVERT, t, &a[1], 1, &ok);
        if (!ok)
            return 0;
        return n[5] == 'n' ? cx_expr_nothrow(e) : 1;
    }
    if (!strcmp(n, "__is_assignable") ||
        !strcmp(n, "__is_trivially_assignable") ||
        !strcmp(n, "__is_nothrow_assignable")) {
        if (!is_object(ct_strip_ref(t)) || !is_object(ct_strip_ref(a[1])))
            return 0;
        int ok;
        struct cexpr *e = try_build(TRY_ASSIGN, t, &a[1], 1, &ok);
        if (!ok)
            return 0;
        if (n[5] == 'n')
            return cx_expr_nothrow(e);
        if (n[5] == 't') {
            struct cty *l = ct_strip_ref(t);
            if (l->k != CT_CLASS)
                return 1;
            return e->k != E_CALL || (e->fn->is_implicit &&
                                      l->cls->trivial_assign);
        }
        return 1;
    }
    if (!strcmp(n, "__is_constructible") ||
        !strcmp(n, "__is_trivially_constructible") ||
        !strcmp(n, "__is_nothrow_constructible")) {
        if (t->k == CT_VOID || t->k == CT_FUNC ||
            (t->k == CT_ARRAY && (t->n < 0 || na > 1)))
            return 0;
        if (t->k == CT_CLASS && !ct_is_complete(t))
            return 0;
        int ok;
        struct cexpr *e = try_build(TRY_CONSTRUCT, t, a + 1, na - 1, &ok);
        if (!ok)
            return 0;
        if (n[5] == 'n')
            return cx_expr_nothrow(e);
        if (n[5] == 't') {
            if (ct_is_ref(t))
                return e && e->k == E_ADDR;
            struct cty *u = t;
            while (u->k == CT_ARRAY)
                u = u->to;
            if (u->k != CT_CLASS)
                return 1;
            if (na == 1)
                return u->cls->trivial_default;
            if (na == 2 && a[1] && ct_same_unqual(ct_strip_ref(a[1]), u))
                return u->cls->trivial_copy;
            return 0;
        }
        return 1;
    }
    return 0;
}

static struct cty *type_trait(const char *n, struct cty *t,
                              const struct ctok *at)
{
    if (!strcmp(n, "__underlying_type")) {
        if (t->k != CT_ENUM)
            cx_error(at, "__underlying_type of %s, which is not an enum",
                     ct_name(t));
        return t->en->underlying ? t->en->underlying : ct_basic(CT_INT);
    }
    if (!strcmp(n, "__remove_cv"))
        return remove_cv(t);
    if (!strcmp(n, "__remove_reference"))
        return ct_strip_ref(t);
    if (!strcmp(n, "__remove_cvref"))
        return remove_cv(ct_strip_ref(t));
    if (!strcmp(n, "__remove_pointer"))
        return t->k == CT_PTR ? t->to : t;
    if (!strcmp(n, "__add_pointer"))
        return ct_is_ref(t) ? ct_ptr(t->to)
               : t->k == CT_FUNC && (t->fq || t->refq) ? t : ct_ptr(t);
    if (!strcmp(n, "__add_lvalue_reference") ||
        !strcmp(n, "__add_rvalue_reference")) {
        if (t->k == CT_VOID || (t->k == CT_FUNC && (t->fq || t->refq)))
            return t;
        return ct_ref(t, n[6] == 'r');
    }
    if (!strcmp(n, "__decay")) {
        struct cty *u = ct_strip_ref(t);
        if (u->k == CT_ARRAY)
            return ct_ptr(u->to);
        if (u->k == CT_FUNC)
            return ct_ptr(u);
        return ct_unqual(u);
    }
    if (!strcmp(n, "__remove_extent"))
        return t->k == CT_ARRAY ? t->to : t;
    if (!strcmp(n, "__remove_all_extents")) {
        while (t->k == CT_ARRAY)
            t = t->to;
        return t;
    }
    return t;
}

/* At a trait's name (trait_at): its value, a bool constant. */
struct cexpr *parse_trait(void)
{
    const struct ctok *at = cx_cur();
    const char *n = at->t.text;
    const struct trait *tr = find_trait(n);
    cx_advance();
    struct cty **a;
    int na = trait_args(&a);
    int want = tr->kind == TR_BOOL2 ? 2 : 1;
    if (tr->kind == TR_BOOLN ? na < 1 : na != want)
        cx_error(at, "'%s' takes %s%d type%s", n,
                 tr->kind == TR_BOOLN ? "at least " : "", want,
                 want == 1 ? "" : "s");
    if (tr->kind == TR_TYPE1)
        cx_error(at, "'%s' is a type, not a value", n);
    for (int i = 0; i < na; i++)
        if (ct_dependent(a[i]))
            return ex_int(0, ct_basic(CT_BOOL));     /* in a pattern */
    return ex_int(bool_trait(n, a, na) != 0, ct_basic(CT_BOOL));
}

/* At a type trait's name (__underlying_type(T) and the like): the type. */
struct cty *parse_type_trait(void)
{
    const struct ctok *at = cx_cur();
    const char *n = at->t.text;
    cx_advance();
    struct cty **a;
    int na = trait_args(&a);
    if (na != 1)
        cx_error(at, "'%s' takes one type", n);
    if (ct_dependent(a[0])) {
        struct cty *d = xcalloc(1, sizeof *d);
        d->k = CT_DEP;
        return d;
    }
    return type_trait(n, a[0], at);
}
