/* The typed C++ trees, written out as C for EmbCC's C front-end.
 *
 * What C has no word for is spelled out: a reference is a pointer (its
 * uses dereference it), a member function takes `this` first, a
 * constructor or destructor is a function called on the object's address
 * (C1/C2, D1/D2: the complete-object one calls the base-object one), a
 * temporary is a variable declared at its full-expression, destroyed at
 * that full-expression's end — or at its block's end once bound to a local
 * reference — and every way out of a scope (its end, return, break,
 * continue, goto) first destroys the objects the scope constructed.
 *
 * Only what is used is written: an inline function, an implicit special
 * member or an internal variable is emitted once something emitted refers
 * to it (a worklist), and a namespace-scope object whose initialization
 * runs code gets it from one function per unit, run from .init_array, with
 * its destructor registered by __cxa_atexit. */
#include "cxx.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../arch/target.h"
#include "../driver/util.h"

/* ---- text buffers ---- */

struct sb {
    char *p;
    size_t len, cap;
};

static void sb_putn(struct sb *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2 + 64;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_put(struct sb *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

static void sb_printf(struct sb *b, const char *fmt, ...)
{
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof small) {
        sb_putn(b, small, (size_t)n);
        return;
    }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_putn(b, big, (size_t)n);
    free(big);
}

static const char *sb_str(struct sb *b)
{
    return b->p ? b->p : "";
}

/* ---- names ---- */

static int is_c_keyword(const char *n)
{
    static const char *const kw[] = {
        "restrict", "typeof", "_Bool", "_Complex", "_Generic", "_Atomic",
        "_Noreturn", "_Alignas", "_Alignof", "_Static_assert",
        "_Thread_local", "_Imaginary",
    };
    for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++)
        if (strcmp(n, kw[i]) == 0)
            return 1;
    return 0;
}

static const char *field_cname(const struct cfield *fl)
{
    return is_c_keyword(fl->name) ? cx_fmt("__cx_f_%s", fl->name) : fl->name;
}

static int fn_internal(const struct cfunc *f)
{
    if (f->local_inst)
        return 1;
    if (f->cls)
        return f->cls->local || f->cls->anon;
    return f->is_static;
}

static const char *fn_name(struct cfunc *f, int variant)
{
    if (f->asm_name)
        return f->asm_name;
    if (f->c_linkage)
        return f->name;
    if (f->local_inst || (f->cls && (f->cls->local || f->cls->anon))) {
        if (!f->cname) {
            f->cname = cx_fmt("__cx_lf%d", cx_uid());
            f->cname2 = cx_fmt("%s_base", f->cname);
            f->cname0 = cx_fmt("%s_del", f->cname);
        }
        if (f->is_ctor || f->is_dtor)
            return variant == 2 ? f->cname2 : variant == 0 ? f->cname0
                                                           : f->cname;
        return f->cname;
    }
    if (f->is_dtor && variant == 0) {
        if (!f->cname0) {
            f->ctor_variant = 0;
            f->cname0 = mangle_func(f);
        }
        return f->cname0;
    }
    if (f->is_ctor || f->is_dtor) {
        if (variant == 2) {
            if (!f->cname2) {
                f->ctor_variant = 2;
                f->cname2 = mangle_func(f);
            }
            return f->cname2;
        }
        if (!f->cname) {
            f->ctor_variant = 1;
            f->cname = mangle_func(f);
        }
        return f->cname;
    }
    if (!f->cname)
        f->cname = mangle_func(f);
    return f->cname;
}

/* ---- RTTI and vtable names ---- */

static int class_internal(const struct cclass *c)
{
    if (c->local || c->anon)
        return 1;
    for (struct cscope *s = c->owner; s; s = s->parent)
        if (s->k == SC_NAMESPACE && s->anon)
            return 1;
    return 0;
}

static const char *class_sym(struct cclass *c, const char *prefix)
{
    if (c->local || c->anon)
        return cx_fmt("__cx_%s%s", prefix + 2, c->cname);
    return cx_fmt("%s%s", prefix, mangle_class_name(c));
}

/* ---- C types ---- */

static const char *basic_c(enum cty_kind k)
{
    int arm = target_get() == TARGET_AARCH64;
    switch (k) {
    case CT_VOID: return "void";
    case CT_BOOL: return "_Bool";
    case CT_CHAR: return "char";
    case CT_SCHAR: return "signed char";
    case CT_UCHAR: case CT_CHAR8: return "unsigned char";
    case CT_WCHAR: return arm ? "unsigned int" : "int";
    case CT_CHAR16: case CT_USHORT: return "unsigned short";
    case CT_CHAR32: case CT_UINT: return "unsigned int";
    case CT_SHORT: return "short";
    case CT_INT: return "int";
    case CT_LONG: return "long";
    case CT_ULONG: return "unsigned long";
    case CT_LLONG: return "long long";
    case CT_ULLONG: return "unsigned long long";
    case CT_FLOAT: return "float";
    case CT_DOUBLE: return "double";
    case CT_LDOUBLE: return "long double";
    case CT_NULLPTR: return "void *";
    case CT_VALIST: return "__builtin_va_list";
    default: return "int";
    }
}

static int need_pmf;            /* the unit uses struct __cx_pmf */

/* t declaring `inner` (a name, or "" for an abstract type), in C. */
static char *cdecl(struct cty *t, const char *inner)
{
    const char *vol = (t->q & CQ_VOLATILE) ? "volatile " : "";
    const char *sp = *inner ? " " : "";
    switch (t->k) {
    case CT_PTR: case CT_LREF: case CT_RREF: {
        char *s = cx_fmt("*%s%s", (t->q & CQ_VOLATILE) ? "volatile " : "",
                         inner);
        if (t->to->k == CT_ARRAY || t->to->k == CT_FUNC)
            s = cx_fmt("(%s)", s);
        return cdecl(t->to, s);
    }
    case CT_ARRAY:
        return cdecl(t->to, t->n >= 0 ? cx_fmt("%s[%ld]", inner, t->n)
                                      : cx_fmt("%s[]", inner));
    case CT_FUNC: {
        /* the ABI's C: a class not trivially copyable is passed as a
         * pointer, and returned through the indirect-result pointer */
        struct sb p = { 0, 0, 0 };
        int sret = class_indirect(t->to), any = sret;
        if (sret)
            sb_printf(&p, "__attribute__((embcc_sret)) %s",
                      cdecl(ct_ptr(t->to), ""));
        for (int i = 0; i < t->np; i++) {
            struct cty *pt = t->params[i];
            sb_printf(&p, "%s%s", any ? ", " : "",
                      cdecl(class_indirect(pt) ? ct_ptr(pt) : pt, ""));
            any = 1;
        }
        if (t->variadic)
            sb_put(&p, any ? ", ..." : "...");
        if (!any && !t->variadic)
            sb_put(&p, "void");
        return cdecl(sret ? ct_ptr(t->to) : t->to,
                     cx_fmt("%s(%s)", inner, sb_str(&p)));
    }
    case CT_CLASS:
        return cx_fmt("%s%s %s%s%s", vol, t->cls->is_union ? "union"
                                                          : "struct",
                      t->cls->cname, sp, inner);
    case CT_ENUM:
        return cdecl(ct_qual(t->en->underlying, t->q), inner);
    case CT_MPTR:
        /* Itanium: a data member's offset (null -1), or a member
         * function's { ptr, adj } */
        if (ct_is_pmf(t)) {
            need_pmf = 1;
            return cx_fmt("%sstruct __cx_pmf%s%s", vol, sp, inner);
        }
        return cx_fmt("%slong%s%s", vol, sp, inner);
    default:
        return cx_fmt("%s%s%s%s", vol, basic_c(t->k), sp, inner);
    }
}

static const char *ctype(struct cty *t)
{
    return cdecl(t, "");
}

/* ---- emission state ---- */

static struct sb out_types, out_decls, out_vars, out_code, out_init;
static struct sb out_rettypes;       /* typedefs of functions' return types */
static struct sb out_rtti;      /* typeinfo objects */
static struct sb out_vtables;
static struct cfunc **work;
static int nwork, capwork;
static struct cfunc *cur_fn;
static int need_atexit, need_guard;

static void need_fn(struct cfunc *f)
{
    /* an implicit or defaulted member a vtable (or a call) reaches */
    if ((f->is_implicit || f->is_defaulted) && !f->defined && !f->is_deleted)
        define_implicit(f);
    if (!f->declared) {
        f->declared = 1;
        f->used = 1;
    }
    if (f->defined && !f->queued) {
        f->queued = 1;
        if (nwork == capwork) {
            capwork = capwork ? capwork * 2 : 64;
            work = xrealloc(work, (size_t)capwork * sizeof *work);
        }
        work[nwork++] = f;
    }
}

static int any_vtable;
static int need_dyncast;
static const char *typeinfo_sym(struct cty *t);
static const char *ptr_typeinfo(struct cty *t);

static void need_vtable(struct cclass *c)
{
    if (c->dynamic && !c->vtable_used) {
        c->vtable_used = 1;
        any_vtable = 1;
    }
}

static void need_var(struct cvar *v)
{
    if (!v->is_local || v->is_static)
        v->refd = 1;
}

/* A variable whose C object is a pointer to the C++ one: a reference, a
 * class parameter passed by reference to the caller's temporary, the
 * named return value living in the caller's return slot. */
static int var_is_ptr(const struct cvar *v)
{
    if (ct_is_ref(v->type) || v->nrvo)
        return 1;
    return v->is_param && class_indirect(v->type);
}

/* The called function's type (a call's value category may have been
 * rewritten by an lvalue-to-rvalue conversion; its declared result
 * decides how C sees it). */
static struct cty *call_ftype(const struct cexpr *e)
{
    if (e->k == E_CALL)
        return e->fn->type;
    if (e->k == E_PMCALL)
        return e->a[1]->t->to;      /* through a pointer to member */
    return e->a[0]->t->to;          /* E_ICALL: through a pointer */
}

/* The call returns a reference: C gets a pointer. */
static int call_ref(const struct cexpr *e)
{
    return ct_is_ref(call_ftype(e)->to);
}

/* A call whose class result the callee builds in a slot the caller
 * passes (the Itanium return of a class not trivially copyable). */
static int call_sret(const struct cexpr *e)
{
    return (e->k == E_CALL || e->k == E_ICALL || e->k == E_PMCALL) &&
           class_indirect(call_ftype(e)->to);
}

/* ---- literals ---- */

static char *int_lit(long v, struct cty *t)
{
    t = ct_unqual(t);
    if (t->k == CT_ENUM)
        t = t->en->underlying;
    switch (t->k) {
    case CT_BOOL:
        return v ? "1" : "0";
    case CT_UINT: case CT_CHAR32:
        return cx_fmt("%luU", (unsigned long)v & 0xffffffffUL);
    case CT_WCHAR:
        if (target_get() == TARGET_AARCH64)
            return cx_fmt("%luU", (unsigned long)v & 0xffffffffUL);
        break;
    case CT_LONG: case CT_LLONG: {
        const char *suf = t->k == CT_LONG ? "L" : "LL";
        if (v == LONG_MIN)
            return cx_fmt("(-9223372036854775807%s-1)", suf);
        return v < 0 ? cx_fmt("(%ld%s)", v, suf) : cx_fmt("%ld%s", v, suf);
    }
    case CT_ULONG:
        return cx_fmt("%luUL", (unsigned long)v);
    case CT_ULLONG:
        return cx_fmt("%luULL", (unsigned long)v);
    default:
        break;
    }
    if (v == INT_MIN)
        return "(-2147483647-1)";
    return v < 0 ? cx_fmt("(%ld)", v) : cx_fmt("%ld", v);
}

static char *flt_lit(struct cexpr *e)
{
    enum cty_kind k = e->t->k;
    const char *suf = k == CT_FLOAT ? "f" : k == CT_LDOUBLE ? "L" : "";
    if (k == CT_LDOUBLE && e->text)
        return cx_fmt("%sL", e->text);
    double d = e->fval;
    if (isinf(d))
        return cx_fmt("%s__builtin_inf%s()", d < 0 ? "-" : "",
                      k == CT_FLOAT ? "f" : k == CT_LDOUBLE ? "l" : "");
    if (isnan(d))
        return cx_fmt("__builtin_nan%s(\"\")",
                      k == CT_FLOAT ? "f" : k == CT_LDOUBLE ? "l" : "");
    char buf[64];
    if (k == CT_FLOAT)
        snprintf(buf, sizeof buf, "%.9g", (double)(float)d);
    else
        snprintf(buf, sizeof buf, "%.17g", d);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&
        !strchr(buf, 'n'))
        strcat(buf, ".0");
    return cx_fmt("%s%s", buf, suf);
}

static char *str_lit(struct cexpr *e)
{
    struct sb b = { 0, 0, 0 };
    int w = e->swidth;
    struct cty *et = e->t->k == CT_ARRAY ? e->t->to : e->t;
    const char *pfx = w == 1 ? "" : w == 2 ? "u"
                      : et->k == CT_WCHAR ? "L" : "U";
    sb_printf(&b, "%s\"", pfx);
    const unsigned char *p = (const unsigned char *)e->text;
    for (long i = 0; i < e->slen - 1; i++) {
        unsigned long v = 0;
        for (int k = 0; k < w; k++)
            v |= (unsigned long)p[i * w + k] << (8 * k);
        if (w == 1) {
            if (v >= 0x20 && v < 0x7f && v != '"' && v != '\\' && v != '?')
                sb_printf(&b, "%c", (int)v);
            else
                sb_printf(&b, "\\%03lo", v);
        } else {
            sb_printf(&b, "\\x%lx", v);
        }
    }
    sb_put(&b, "\"");
    return b.p;
}

/* ---- full-expressions and their temporaries ---- */

/* ---- exception regions ---- */

/* C++ exceptions are on: what a throw must undo is put in regions of
 * EmbCC's C (__builtin_eh_region ... __builtin_eh_landing), each landing
 * pad doing its part and handing on to the enclosing one — by goto, to
 * its label __cx_lp<n>, as resuming inside the frame that handles the
 * exception would find the same pad again — or, from the outermost, to
 * the caller (_Unwind_Resume). */
static int eh_on;
static int fn_eh;             /* this function has regions: its __cx_exc
                               * and __cx_sel */
static int eh_used;           /* the unit calls the ABI's EH functions */
static int eh_lp[1024];       /* the open regions' pads, innermost last */
static int neh_lp;

static int eh_open(struct sb *b)
{
    int lp = cx_uid();
    if (neh_lp == 1024)
        cx_error(NULL, "exception regions nested too deeply");
    eh_lp[neh_lp++] = lp;
    fn_eh = 1;
    eh_used = 1;
    sb_put(b, "__builtin_eh_region {\n");
    return lp;
}

/* After a pad's own work: the enclosing pad's, or the caller's. */
static void eh_chain(struct sb *b)
{
    if (neh_lp)
        sb_printf(b, "goto __cx_lp%d;\n", eh_lp[neh_lp - 1]);
    else
        sb_put(b, "((void (*)(void *))_Unwind_Resume)(__cx_exc); "
                  "__builtin_unreachable();\n");
}

/* Close region lp as a cleanup: its pad runs text, then hands on. */
static void eh_close(struct sb *b, int lp, const char *text)
{
    if (!neh_lp || eh_lp[neh_lp - 1] != lp)
        cx_error(NULL, "internal: exception regions closed out of order");
    neh_lp--;
    sb_printf(b, "} __builtin_eh_landing (__cx_exc, __cx_sel, __eh_cleanup) "
                 "{\n__cx_lp%d:;\n%s\n", lp, text);
    eh_chain(b);
    sb_put(b, "}\n");
}

struct fx {
    struct sb decls;          /* the temporaries' declarations */
    char **clean;             /* their destruction, in construction order */
    int nclean, capclean;
    int cond;                 /* inside a conditionally evaluated operand */
};

static struct fx *fx;
static int ntemps;

static char *ev(struct cexpr *e);
static char *elv(struct cexpr *e);
static char *eaddr(struct cexpr *e);
static const char *vtt_arg(struct cfunc *f, int baseobj);
static char *elem_init(const char *arr, int u, struct cty *el,
                       struct cexpr *init);
static void einit(struct sb *b, const char *dest, struct cty *t,
                  struct cexpr *init);

/* The destructor call text for the object at address `addr` of type t
 * (arrays element by element, last first), or NULL when trivial. */
static char *destroy_text(const char *addr, struct cty *t)
{
    if (t->k == CT_ARRAY) {
        long n = 1;
        struct cty *e = t;
        while (e->k == CT_ARRAY) {
            n *= e->n;
            e = e->to;
        }
        if (e->k != CT_CLASS)
            return NULL;
        struct cfunc *d = class_dtor(e->cls);
        if (!d)
            return NULL;
        need_fn(d);
        int u = cx_uid();
        return cx_fmt("{ %s = (%s)(%s); for (unsigned long __cx_i%d = %ldUL; "
                      "__cx_i%d-- > 0; ) %s(&__cx_p%d[__cx_i%d]); }",
                      cdecl(ct_ptr(e), cx_fmt("__cx_p%d", u)),
                      ctype(ct_ptr(e)), addr, u, n, u, fn_name(d, 1), u, u);
    }
    if (t->k != CT_CLASS)
        return NULL;
    struct cfunc *d = class_dtor(t->cls);
    if (!d)
        return NULL;
    need_fn(d);
    return cx_fmt("%s(%s);", fn_name(d, 1), addr);
}

static void fx_clean(const char *text)
{
    if (fx->nclean == fx->capclean) {
        fx->capclean = fx->capclean ? fx->capclean * 2 : 8;
        fx->clean = xrealloc(fx->clean, (size_t)fx->capclean *
                                        sizeof *fx->clean);
    }
    fx->clean[fx->nclean++] = cx_strdup(text);
}

/* A temporary of type t initialized by init, destroyed at the end of the
 * full-expression: its lvalue. */
static char *materialize(struct cty *t, struct cexpr *init)
{
    t = ct_unqual(t);
    char *name = cx_fmt("__cx_t%d", ++ntemps);
    sb_printf(&fx->decls, "%s; ", cdecl(t, name));
    struct sb s = { 0, 0, 0 };
    einit(&s, name, t, init);
    char *d = destroy_text(cx_fmt("&%s", name), t);
    if (d) {
        if (fx->cond || eh_on) {   /* a throw may come before it is made */
            char *flag = cx_fmt("__cx_f%d", ntemps);
            sb_printf(&fx->decls, "_Bool %s = 0; ", flag);
            sb_printf(&s, "%s = 1; ", flag);
            fx_clean(cx_fmt("if (%s) %s", flag, d));
        } else {
            fx_clean(d);
        }
    }
    return cx_fmt("(*({ %s &%s; }))", sb_str(&s), name);
}

static void fx_begin(struct fx *f, struct fx **save)
{
    memset(f, 0, sizeof *f);
    *save = fx;
    fx = f;
}

static void fx_end(struct fx *save)
{
    fx = save;
}

static char *fx_cleanup(struct fx *f)
{
    struct sb b = { 0, 0, 0 };
    for (int i = f->nclean - 1; i >= 0; i--)
        sb_printf(&b, "%s ", f->clean[i]);
    return b.p ? b.p : "";
}

/* e (of type t) as a complete value: temporaries it makes live and die
 * within it. */
static char *full_value(struct cexpr *e)
{
    struct fx f, *save;
    fx_begin(&f, &save);
    char *v = ev(e);
    fx_end(save);
    if (!f.decls.len && !f.nclean)
        return v;
    if (eh_on && f.nclean) {
        /* the temporaries made so far destroyed if it throws */
        struct sb b = { 0, 0, 0 };
        char *clean = fx_cleanup(&f);
        int isvoid = e->t->k == CT_VOID;
        sb_printf(&b, "({ %s", sb_str(&f.decls));
        if (!isvoid)
            sb_printf(&b, "%s; ", cdecl(ct_unqual(e->t), "__cx_r"));
        int lp = eh_open(&b);
        sb_printf(&b, isvoid ? "%s;\n" : "__cx_r = %s;\n", v);
        eh_close(&b, lp, clean);
        sb_printf(&b, "%s%s})", clean, isvoid ? "" : "__cx_r; ");
        return b.p;
    }
    if (e->t->k == CT_VOID)
        return cx_fmt("({ %s%s; %s})", sb_str(&f.decls), v, fx_cleanup(&f));
    return cx_fmt("({ %s%s = %s; %s__cx_r; })", sb_str(&f.decls),
                  cdecl(ct_unqual(ct_decay(e->t)), "__cx_r"), v,
                  fx_cleanup(&f));
}

/* e evaluated for its effects, as a statement. */
static void full_stmt(struct sb *b, struct cexpr *e)
{
    struct fx f, *save;
    fx_begin(&f, &save);
    char *v = ev(e);
    fx_end(save);
    if (!f.decls.len && !f.nclean) {
        sb_printf(b, "%s;\n", v);
        return;
    }
    if (eh_on && f.nclean) {
        char *clean = fx_cleanup(&f);
        sb_printf(b, "{ %s", sb_str(&f.decls));
        int lp = eh_open(b);
        sb_printf(b, "(void)%s;\n", v);
        eh_close(b, lp, clean);
        sb_printf(b, "%s}\n", clean);
        return;
    }
    sb_printf(b, "{ %s(void)%s; %s}\n", sb_str(&f.decls), v, fx_cleanup(&f));
}

/* Initialize dest from init as a statement, its temporaries around it. */
static void stmt_init(struct sb *b, const char *dest, struct cty *t,
                      struct cexpr *init)
{
    struct fx f, *save;
    fx_begin(&f, &save);
    struct sb s = { 0, 0, 0 };
    einit(&s, dest, t, init);
    fx_end(save);
    if (!f.decls.len && !f.nclean) {
        sb_printf(b, "%s\n", sb_str(&s));
        return;
    }
    if (eh_on && f.nclean) {
        char *clean = fx_cleanup(&f);
        sb_printf(b, "{ %s", sb_str(&f.decls));
        int lp = eh_open(b);
        sb_printf(b, "%s\n", sb_str(&s));
        eh_close(b, lp, clean);
        sb_printf(b, "%s}\n", clean);
        return;
    }
    sb_printf(b, "{ %s%s %s}\n", sb_str(&f.decls), sb_str(&s),
              fx_cleanup(&f));
}

/* ---- expressions ---- */

static char *args_text(struct cexpr **a, int n, int from)
{
    struct sb b = { 0, 0, 0 };
    for (int i = from; i < n; i++)
        sb_printf(&b, "%s%s", i > from ? ", " : "", ev(a[i]));
    return b.p ? b.p : "";
}

static char *pmcall_text(struct cexpr *e, const char *dest);

/* The address of the base an E_BASE reaches through a virtual base, from
 * p (a char * naming the object): the virtual base's offset read from the
 * object's vtable, then the rest of the path. */
static char *vbase_addr(const char *p, struct cexpr *e)
{
    return cx_fmt("%s + *(long *)(*(char **)%s + %ldL) + %ldL", p, p,
                  e->vbindex, e->ival);
}

/* The call, its result slot first when it has one (dest: an lvalue). */
static char *call_text(struct cexpr *e, const char *dest)
{
    if (e->k == E_PMCALL)
        return pmcall_text(e, dest);
    const char *slot = dest ? cx_fmt("&(%s)%s", dest,
                                     e->na > (e->k == E_ICALL) ? ", " : "")
                            : "";
    if (e->k == E_ICALL)
        return cx_fmt("(%s)(%s%s)", ev(e->a[0]), slot,
                      args_text(e->a, e->na, 1));
    struct cfunc *f = e->fn;
    if (f->is_virtual && f->cls && !f->is_static && !e->nonvirt &&
        f->vslot >= 0) {
        /* through the object's vptr: slot vslot of its class's vtable (a
         * destructor's D1; its deleting D0 is the next) */
        struct cty **ps = xmalloc((size_t)(f->type->np + 1) * sizeof *ps);
        ps[0] = ct_ptr(ct_class(f->cls));
        for (int i = 0; i < f->type->np; i++)
            ps[i + 1] = f->type->params[i];
        struct cty *cft = ct_func(f->type->to, ps, f->type->np + 1,
                                  f->type->variadic);
        int u = cx_uid();
        need_vtable(f->cls);
        return cx_fmt("({ %s = %s; ((%s)((*(void ***)__cx_v%d)[%d]))(%s"
                      "__cx_v%d%s%s); })", cdecl(ps[0], cx_fmt("__cx_v%d", u)),
                      ev(e->a[0]), ctype(ct_ptr(cft)), u, f->vslot, slot, u,
                      e->na > 1 ? ", " : "", args_text(e->a, e->na, 1));
    }
    need_fn(f);
    if (e->baseobj && f->cls && f->cls->nvbases && (f->is_ctor || f->is_dtor))
        cx_error(NULL, "internal: a base-object call of '%s' outside a "
                       "constructor", f->name);
    return cx_fmt("%s(%s%s)", fn_name(f, e->baseobj ? 2 : 1), slot,
                  args_text(e->a, e->na, 0));
}

static const char *binop_text(int op)
{
    switch (op) {
    case TOK_PLUS: return "+"; case TOK_MINUS: return "-";
    case TOK_STAR: return "*"; case TOK_SLASH: return "/";
    case TOK_PERCENT: return "%"; case TOK_AMP: return "&";
    case TOK_PIPE: return "|"; case TOK_CARET: return "^";
    case TOK_SHL: return "<<"; case TOK_SHR: return ">>";
    case TOK_EQEQ: return "=="; case TOK_NEQ: return "!=";
    case TOK_LT: return "<"; case TOK_GT: return ">";
    case TOK_LE: return "<="; case TOK_GE: return ">=";
    case TOK_ANDAND: return "&&"; case TOK_OROR: return "||";
    case TOK_TILDE: return "~"; case TOK_BANG: return "!";
    default: return "?";
    }
}

static char *member_text(struct cexpr *e)
{
    struct cexpr *o = e->a[0];
    const char *f = field_cname(e->field);
    char *t = o->k == E_DEREF ? cx_fmt("(%s)->%s", ev(o->a[0]), f)
                              : cx_fmt("(%s).%s", elv(o), f);
    if (ct_is_ref(e->field->type))
        return cx_fmt("(*%s)", t);
    return t;
}

/* (obj->*pmf)(args): adjust `this` by adj; an odd ptr is 1 + a vtable
 * offset (a virtual function), else the function itself. */
static char *pmcall_text(struct cexpr *e, const char *dest)
{
    struct cty *pmt = e->a[1]->t, *ft = pmt->to;
    struct cty **ps = xmalloc((size_t)(ft->np + 1) * sizeof *ps);
    ps[0] = ct_ptr(ct_class(pmt->cls));
    for (int i = 0; i < ft->np; i++)
        ps[i + 1] = ft->params[i];
    struct cty *cft = ct_func(ft->to, ps, ft->np + 1, ft->variadic);
    int u = cx_uid();
    struct sb b = { 0, 0, 0 };
    need_pmf = 1;
    sb_printf(&b, "({ struct __cx_pmf __cx_m%d = %s; char *__cx_o%d = "
                  "(char *)%s + __cx_m%d.adj; ", u, ev(e->a[1]), u,
              ev(e->a[0]), u);
    sb_printf(&b, "((%s)(((long)__cx_m%d.ptr & 1) ? *(void **)(*(char **)"
                  "__cx_o%d + (long)__cx_m%d.ptr - 1) : __cx_m%d.ptr))(",
              ctype(ct_ptr(cft)), u, u, u, u);
    if (dest)
        sb_printf(&b, "&(%s), ", dest);
    sb_printf(&b, "(%s)__cx_o%d%s%s); })", ctype(ps[0]), u,
              e->na > 2 ? ", " : "", args_text(e->a, e->na, 2));
    return b.p;
}

/* throw x: the exception object made in __cxa_allocate_exception's
 * memory, then __cxa_throw with its typeinfo and destructor; throw;
 * rethrows the one being handled. */
static char *throw_text(struct cexpr *e)
{
    eh_used = 1;
    if (!e->na)
        return cx_fmt("({ ((void (*)(void))__cxa_rethrow)(); "
                      "__builtin_unreachable(); })");
    struct cty *t = e->alloc_t;
    int u = cx_uid();
    char *x = cx_fmt("__cx_x%d", u);
    struct sb b = { 0, 0, 0 };
    sb_printf(&b, "({ void *%s = ((void *(*)(unsigned long))"
                  "__cxa_allocate_exception)(%ldUL); ", x, ct_size(t));
    einit(&b, cx_fmt("(*(%s)%s)", ctype(ct_ptr(t)), x), t, e->a[0]);
    struct cfunc *d = t->k == CT_CLASS ? class_dtor(t->cls) : NULL;
    if (d)
        need_fn(d);
    sb_printf(&b, "((void (*)(void *, void *, void (*)(void *)))__cxa_throw)"
                  "(%s, (void *)%s, (void (*)(void *))%s); "
                  "__builtin_unreachable(); })", x, typeinfo_sym(t),
              d ? fn_name(d, 1) : "0");
    return b.p;
}

static char *new_text(struct cexpr *e)
{
    need_fn(e->fn);
    int u = cx_uid();
    struct cty *el = e->alloc_t;
    struct cty *pt = ct_ptr(el);
    char *p = cx_fmt("__cx_p%d", u);
    struct sb b = { 0, 0, 0 };
    if (!e->is_array) {
        sb_printf(&b, "({ %s = (%s)%s(%s); ", cdecl(pt, p), ctype(pt),
                  fn_name(e->fn, 1), args_text(e->a, e->na, 0));
        if (e->init) {
            struct sb s = { 0, 0, 0 };
            /* if the constructor throws, the storage goes back to the
             * matching operator delete (not after placement new) */
            int lp = -1;
            const char *free_it = NULL;
            if (eh_on && e->na == 1 && el->k == CT_CLASS) {
                struct cexpr *vp = ex_new(E_NULLPTR, ct_ptr(ct_basic(CT_VOID)),
                                          VC_PRVALUE);
                struct cexpr *sz = ex_int(ct_size(el), ct_size_t());
                struct cexpr *oa[2] = { vp, sz };
                struct cexpr *call = call_delete_op(el->cls, oa);
                need_fn(call->fn);
                free_it = cx_fmt("%s(%s%s);", fn_name(call->fn, 1), p,
                                 call->fn->type->np == 2
                                 ? cx_fmt(", %ldUL", ct_size(el)) : "");
                lp = eh_open(&s);
            }
            einit(&s, cx_fmt("(*%s)", p), el, e->init);
            if (lp >= 0)
                eh_close(&s, lp, free_it);
            if (e->na > 1)
                sb_printf(&b, "if (%s) { %s} ", p, sb_str(&s));
            else
                sb_put(&b, sb_str(&s));
        }
        sb_printf(&b, "%s; })", p);
        return b.p;
    }
    /* new T[n]: [cookie holding n] then the elements */
    char *n = cx_fmt("__cx_n%d", u), *m = cx_fmt("__cx_m%d", u);
    sb_printf(&b, "({ unsigned long %s = %s; char *%s = (char *)%s(%s * "
                  "%ldUL + %ldUL", n, ev(e->count), m, fn_name(e->fn, 1), n,
              ct_size(el), e->cookie);
    for (int i = 1; i < e->na; i++)
        sb_printf(&b, ", %s", ev(e->a[i]));
    sb_put(&b, "); ");
    if (e->cookie)
        sb_printf(&b, "*(unsigned long *)(%s + %ldUL) = %s; ", m,
                  e->cookie - 8, n);
    sb_printf(&b, "%s = (%s)(%s + %ldUL); ", cdecl(pt, p), ctype(pt), m,
              e->cookie);
    if (e->init)
        sb_printf(&b, "for (unsigned long __cx_i%d = 0; __cx_i%d < %s; "
                      "__cx_i%d++) { %s} ", u, u, n, u,
                  elem_init(p, u, el, e->init));
    sb_printf(&b, "%s; })", p);
    return b.p;
}

static char *delete_text(struct cexpr *e)
{
    need_fn(e->fn);
    int u = cx_uid();
    struct cty *el = e->alloc_t;
    struct cty *pt = ct_ptr(el);
    char *p = cx_fmt("__cx_p%d", u);
    int sized = e->fn->type->np == 2;
    struct sb b = { 0, 0, 0 };
    sb_printf(&b, "({ %s = %s; if (%s) { ", cdecl(pt, p), ev(e->a[0]), p);
    if (!e->is_array && e->dtor && e->dtor->is_virtual) {
        /* the deleting destructor of the object's own class: D0, the
         * slot after D1 — it frees with the right size */
        need_vtable(e->dtor->cls);
        sb_printf(&b, "((void (*)(%s))((*(void ***)%s)[%d]))(%s); ",
                  ctype(pt), p, e->dtor->vslot + 1, p);
        sb_put(&b, "} (void)0; })");
        return b.p;
    }
    if (!e->is_array) {
        if (e->dtor) {
            need_fn(e->dtor);
            sb_printf(&b, "%s(%s); ", fn_name(e->dtor, 1), p);
        }
        if (sized)
            sb_printf(&b, "%s(%s, %ldUL); ", fn_name(e->fn, 1), p,
                      ct_size(el));
        else
            sb_printf(&b, "%s(%s); ", fn_name(e->fn, 1), p);
    } else if (e->cookie) {
        need_fn(e->dtor);
        sb_printf(&b, "unsigned long __cx_n%d = *(unsigned long *)((char *)"
                      "%s - 8); for (unsigned long __cx_i%d = __cx_n%d; "
                      "__cx_i%d-- > 0; ) %s(&%s[__cx_i%d]); ", u, p, u, u, u,
                  fn_name(e->dtor, 1), p, u);
        if (sized)
            sb_printf(&b, "%s((char *)%s - %ldUL, __cx_n%d * %ldUL + %ldUL); ",
                      fn_name(e->fn, 1), p, e->cookie, u, ct_size(el),
                      e->cookie);
        else
            sb_printf(&b, "%s((char *)%s - %ldUL); ", fn_name(e->fn, 1), p,
                      e->cookie);
    } else {
        sb_printf(&b, "%s(%s); ", fn_name(e->fn, 1), p);
    }
    sb_put(&b, "} (void)0; })");
    return b.p;
}

static void emit_stmt(struct sb *b, struct cstmt *s);
static void emit_block_items(struct sb *b, struct cstmt *first);

/* e's value, in C. */
static char *ev(struct cexpr *e)
{
    long v;
    if (e->t && ct_is_integer(e->t) && e->k != E_INT && expr_fold(e, &v))
        return int_lit(v, e->t);
    switch (e->k) {
    case E_INT:
        return int_lit(e->ival, e->t);
    case E_FLT:
        return flt_lit(e);
    case E_STR:
        return str_lit(e);
    case E_NULLPTR:
        return "((void *)0)";
    case E_VAR: case E_DEREF: case E_MEMBER: case E_TEMP:
        return elv(e);
    case E_FUNC:
        need_fn(e->fn);
        return (char *)fn_name(e->fn, 1);
    case E_OVL:
        cx_error(NULL, "internal: an unresolved overload set reached C");
        return NULL;
    case E_CALL: case E_ICALL: case E_PMCALL: {
        if (call_sret(e))
            return materialize(e->t, e);
        char *t = call_text(e, NULL);
        if (call_ref(e))
            return cx_fmt("(*%s)", t);
        return t;
    }
    case E_MEMPTR:
        if (e->field)
            return cx_fmt("%ldL", e->field->off);
        need_fn(e->fn);
        need_pmf = 1;
        return cx_fmt("((struct __cx_pmf){ (void *)%s, 0 })",
                      fn_name(e->fn, 1));
    case E_PMEM: case E_TYPEID:
        return elv(e);
    case E_DYNCAST: {
        int u = cx_uid();
        need_dyncast = 1;
        if (e->is_array)        /* to void *: the object's start */
            return cx_fmt("({ char *__cx_p%d = (char *)%s; __cx_p%d ? (void *)"
                          "(__cx_p%d + ((long *)*(void ***)__cx_p%d)[-2]) : "
                          "(void *)0; })", u, ev(e->a[0]), u, u, u);
        return cx_fmt("({ void *__cx_p%d = (void *)%s; __cx_p%d = __cx_p%d ? "
                      "__dynamic_cast(__cx_p%d, %s, %s, %ldL) : (void *)0; "
                      "%s(%s)__cx_p%d; })", u, ev(e->a[0]), u, u, u,
                      typeinfo_sym(ct_class(e->alloc_t->cls)),
                      typeinfo_sym(e->t->to), e->ival,
                      e->zero ? cx_fmt("if (!__cx_p%d) __cxa_bad_cast(); ", u)
                              : "", ctype(e->t), u);
    }
    case E_MPCONV: {
        int u = cx_uid();
        if (ct_is_pmf(e->t)) {
            need_pmf = 1;
            return cx_fmt("({ struct __cx_pmf __cx_m%d = %s; __cx_m%d.adj += "
                          "%ldL; __cx_m%d; })", u, ev(e->a[0]), u, e->ival, u);
        }
        return cx_fmt("({ long __cx_m%d = %s; __cx_m%d == -1L ? -1L : "
                      "__cx_m%d + %ldL; })", u, ev(e->a[0]), u, u, e->ival);
    }
    case E_BASE:
        if (e->is_array && e->vbindex) {
            /* through a virtual base: its offset is in the vtable */
            int u = cx_uid();
            return cx_fmt("({ char *__cx_b%d = (char *)%s; (%s)(__cx_b%d ? "
                          "%s : 0); })", u, ev(e->a[0]), ctype(e->t), u,
                          vbase_addr(cx_fmt("__cx_b%d", u), e));
        }
        if (e->is_array) {
            /* a pointer: null stays null */
            struct cexpr *o = e->a[0];
            if (!e->ival)
                return cx_fmt("((%s)%s)", ctype(e->t), ev(o));
            if (o->k == E_THIS || o->k == E_ADDR)
                return cx_fmt("((%s)((char *)%s + %ldL))", ctype(e->t),
                              ev(o), e->ival);
            int u = cx_uid();
            return cx_fmt("({ char *__cx_b%d = (char *)%s; (%s)(__cx_b%d ? "
                          "__cx_b%d + %ldL : 0); })", u, ev(o), ctype(e->t),
                          u, u, e->ival);
        }
        return elv(e);
    case E_BUILTIN: {
        if (strcmp(e->name, "__builtin_is_constant_evaluated") == 0)
            return "((_Bool)0)";       /* run time: not constant evaluation */
        struct sb b = { 0, 0, 0 };
        int va = strncmp(e->name, "__builtin_va_", 13) == 0;
        for (int i = 0; i < e->na; i++)
            sb_printf(&b, "%s%s", i ? ", " : "",
                      va && (i == 0 || strcmp(e->name, "__builtin_va_copy")
                                       == 0) ? elv(e->a[i]) : ev(e->a[i]));
        return cx_fmt("%s(%s)", e->name, sb_str(&b));
    }
    case E_UNARY:
        return cx_fmt("(%s%s)", binop_text(e->op), ev(e->a[0]));
    case E_BINARY: {
        if (e->op == TOK_ANDAND || e->op == TOK_OROR) {
            char *l = ev(e->a[0]);
            fx->cond++;
            char *r = ev(e->a[1]);
            fx->cond--;
            return cx_fmt("(%s %s %s)", l, binop_text(e->op), r);
        }
        if (ct_is_pmf(e->a[0]->t)) {
            /* equal: the same ptr, and — unless null — the same adj */
            int u = cx_uid();
            return cx_fmt("({ struct __cx_pmf __cx_a%d = %s, __cx_b%d = %s; "
                          "%s(__cx_a%d.ptr == __cx_b%d.ptr && (!__cx_a%d.ptr "
                          "|| __cx_a%d.adj == __cx_b%d.adj)); })", u,
                          ev(e->a[0]), u, ev(e->a[1]),
                          e->op == TOK_NEQ ? "!" : "", u, u, u, u, u);
        }
        return cx_fmt("(%s %s %s)", ev(e->a[0]), binop_text(e->op),
                      ev(e->a[1]));
    }
    case E_ASSIGN:
        if (e->op == TOK_ASSIGN && e->t->k == CT_CLASS &&
            (e->a[0]->k == E_BASE || e->t->cls->dsize < e->t->cls->size)) {
            /* a trivial assignment writes the data, never tail padding a
             * derived class may be using */
            int u = cx_uid();
            struct cty *pt = ct_ptr(ct_unqual(e->t));
            return cx_fmt("(*({ %s = %s; __builtin_memcpy(__cx_d%d, %s, "
                          "%ldUL); __cx_d%d; }))",
                          cdecl(pt, cx_fmt("__cx_d%d", u)), eaddr(e->a[0]),
                          u, eaddr(e->a[1]), e->t->cls->dsize, u);
        }
        if (e->op == TOK_ASSIGN)
            return cx_fmt("(%s = %s)", elv(e->a[0]), ev(e->a[1]));
        return cx_fmt("(%s %s= %s)", elv(e->a[0]), binop_text(e->op),
                      ev(e->a[1]));
    case E_INCDEC: {
        const char *op = e->ival > 0 ? "++" : "--";
        return e->post ? cx_fmt("(%s%s)", elv(e->a[0]), op)
                       : cx_fmt("(%s%s)", op, elv(e->a[0]));
    }
    case E_COND: {
        char *c = ev(e->a[0]);
        fx->cond++;
        char *a = ev(e->a[1]), *b = ev(e->a[2]);
        fx->cond--;
        return cx_fmt("(%s ? %s : %s)", c, a, b);
    }
    case E_COMMA:
        return cx_fmt("((void)%s, %s)", ev(e->a[0]), ev(e->a[1]));
    case E_CAST:
        if (e->lvcast)
            return elv(e);
        if (e->t->k == CT_VOID)
            return cx_fmt("((void)%s)", ev(e->a[0]));
        if (e->t->k == CT_MPTR) {
            if (e->a[0]->t->k == CT_MPTR)
                return ev(e->a[0]);
            (void)ev(e->a[0]);                /* a null constant */
            need_pmf |= ct_is_pmf(e->t);
            return ct_is_pmf(e->t) ? "((struct __cx_pmf){ 0, 0 })" : "(-1L)";
        }
        if (e->t->k == CT_BOOL && e->a[0]->t->k == CT_MPTR)
            return ct_is_pmf(e->a[0]->t)
                   ? cx_fmt("((%s).ptr != 0)", ev(e->a[0]))
                   : cx_fmt("(%s != -1L)", ev(e->a[0]));
        return cx_fmt("((%s)%s)", ctype(e->t), ev(e->a[0]));
    case E_ADDR:
        return eaddr(e->a[0]);
    case E_THIS:
        return "this";
    case E_CONSTRUCT:
        if (!e->fn && !e->zero && e->na == 1)
            return ev(e->a[0]);
        return materialize(e->t, e);
    case E_INITLIST:
        return materialize(e->t, e);
    case E_NEW:
        return new_text(e);
    case E_THROW:
        return throw_text(e);
    case E_EXCOBJ:               /* __cxa_begin_catch's result, in a handler */
        if (e->is_array)
            return cx_fmt("((%s)__cx_c)", ctype(e->t));
        return cx_fmt("(*(%s)__cx_c)", ctype(ct_ptr(e->t)));
    case E_DELETE:
        return delete_text(e);
    case E_STMTEXPR: {
        struct sb b = { 0, 0, 0 };
        emit_block_items(&b, e->body->body);
        return cx_fmt("({ %s})", sb_str(&b));
    }
    case E_VAARG:
        return cx_fmt("__builtin_va_arg(%s, %s)", elv(e->a[0]), ctype(e->t));
    }
    return "0";
}

/* e (a glvalue) as a C lvalue. */
static char *elv(struct cexpr *e)
{
    switch (e->k) {
    case E_EXCOBJ:               /* the caught object itself, in a handler */
        return cx_fmt("(*(%s)__cx_c)", ctype(ct_ptr(e->t)));
    case E_VAR:
        need_var(e->var);
        if (var_is_ptr(e->var))
            return cx_fmt("(*%s)", e->var->cname);
        return (char *)e->var->cname;
    case E_DEREF:
        return cx_fmt("(*%s)", ev(e->a[0]));
    case E_MEMBER:
        return member_text(e);
    case E_TEMP:
        return materialize(e->t, e->a[0]);
    case E_STR:
        return str_lit(e);
    case E_CALL: case E_ICALL: case E_PMCALL:
        if (!call_ref(e))
            return materialize(e->t, e);
        return ev(e);
    case E_PMEM:
        return cx_fmt("(*(%s)((char *)%s + %s))", ctype(ct_ptr(e->t)),
                      eaddr(e->a[0]), ev(e->a[1]));
    case E_BASE:
        if (!e->is_array && e->vbindex) {
            int u = cx_uid();
            return cx_fmt("(*({ char *__cx_b%d = (char *)%s; (%s)(%s); }))", u,
                          eaddr(e->a[0]), ctype(ct_ptr(e->t)),
                          vbase_addr(cx_fmt("__cx_b%d", u), e));
        }
        if (!e->is_array)
            return cx_fmt("(*(%s)((char *)%s + %ldL))", ctype(ct_ptr(e->t)),
                          eaddr(e->a[0]), e->ival);
        break;
    case E_TYPEID:
        if (e->na)              /* the vtable's slot -1 */
            return cx_fmt("(*(%s)((*(void ***)%s)[-1]))",
                          ctype(ct_ptr(e->t)), eaddr(e->a[0]));
        return cx_fmt("(*(%s)%s)", ctype(ct_ptr(e->t)),
                      typeinfo_sym(e->alloc_t));
    case E_CAST:
        if (e->lvcast) {
            if (ct_same_unqual(e->t, e->a[0]->t))
                return elv(e->a[0]);
            return cx_fmt("(*(%s)%s)", ctype(ct_ptr(e->t)), eaddr(e->a[0]));
        }
        break;
    case E_ASSIGN: case E_INCDEC: {
        /* C gives these a value; C++ an lvalue: the object's address */
        struct cexpr *o = e->a[0];
        int u = cx_uid();
        struct cty *pt = ct_ptr(o->t);
        char *op;
        if (e->k == E_INCDEC)
            op = cx_fmt("%s*__cx_q%d", e->ival > 0 ? "++" : "--", u);
        else if (e->op == TOK_ASSIGN)
            op = cx_fmt("*__cx_q%d = %s", u, ev(e->a[1]));
        else
            op = cx_fmt("*__cx_q%d %s= %s", u, binop_text(e->op),
                        ev(e->a[1]));
        return cx_fmt("(*({ %s = %s; %s; __cx_q%d; }))",
                      cdecl(pt, cx_fmt("__cx_q%d", u)), eaddr(o), op, u);
    }
    case E_COMMA:
        return cx_fmt("(*((void)%s, %s))", ev(e->a[0]), eaddr(e->a[1]));
    case E_COND: {
        char *c = ev(e->a[0]);
        fx->cond++;
        char *a = eaddr(e->a[1]), *b = eaddr(e->a[2]);
        fx->cond--;
        return cx_fmt("(*(%s ? %s : %s))", c, a, b);
    }
    default:
        break;
    }
    if (e->t && e->t->k == CT_CLASS)
        return materialize(e->t, e);
    return ev(e);
}

/* The address of the glvalue e. */
static char *eaddr(struct cexpr *e)
{
    switch (e->k) {
    case E_EXCOBJ:
        return cx_fmt("((%s)__cx_c)", ctype(ct_ptr(e->t)));
    case E_VAR:
        need_var(e->var);
        if (var_is_ptr(e->var))
            return (char *)e->var->cname;
        return cx_fmt("(&%s)", e->var->cname);
    case E_DEREF:
        return ev(e->a[0]);
    case E_MEMBER:
        if (ct_is_ref(e->field->type)) {
            struct cexpr *o = e->a[0];
            const char *f = field_cname(e->field);
            return o->k == E_DEREF ? cx_fmt("(%s)->%s", ev(o->a[0]), f)
                                   : cx_fmt("(%s).%s", elv(o), f);
        }
        break;
    case E_CALL: case E_ICALL: case E_PMCALL:
        if (call_ref(e))
            return call_text(e, NULL);
        break;
    case E_PMEM:
        return cx_fmt("((%s)((char *)%s + %s))", ctype(ct_ptr(e->t)),
                      eaddr(e->a[0]), ev(e->a[1]));
    case E_BASE:
        if (!e->is_array && e->vbindex) {
            int u = cx_uid();
            return cx_fmt("({ char *__cx_b%d = (char *)%s; (%s)(%s); })", u,
                          eaddr(e->a[0]), ctype(ct_ptr(e->t)),
                          vbase_addr(cx_fmt("__cx_b%d", u), e));
        }
        if (!e->is_array)
            return cx_fmt("((%s)((char *)%s + %ldL))", ctype(ct_ptr(e->t)),
                          eaddr(e->a[0]), e->ival);
        break;
    case E_CAST:
        if (e->lvcast)
            return cx_fmt("((%s)%s)", ctype(ct_ptr(e->t)), eaddr(e->a[0]));
        break;
    case E_STR:
        /* a string literal is an lvalue array: its address, converted
         * (the C side takes no `&"..."`) */
        return cx_fmt("((%s)%s)", ctype(ct_ptr(e->t)), str_lit(e));
    case E_TEMP: {
        char *lv = materialize(e->t, e->a[0]);
        /* (*({ ...; &t; })) -> ({ ...; &t; }) */
        size_t n = strlen(lv);
        return cx_fmt("%.*s", (int)(n - 3), lv + 2);
    }
    default:
        break;
    }
    return cx_fmt("(&%s)", elv(e));
}

/* Initialize the object named by the C lvalue `dest` (of type t) from
 * init, as statements. A reference's dest is its pointer. */
/* The body of a loop building element __cx_i<u> of array `arr` (a
 * pointer to its first element) from init: if a constructor throws, the
 * elements already built are destroyed, last first. */
static char *elem_init(const char *arr, int u, struct cty *el,
                       struct cexpr *init)
{
    struct sb s = { 0, 0, 0 };
    char *undo = eh_on ? destroy_text(cx_fmt("&%s[__cx_i%d]", arr, u), el)
                       : NULL;
    int lp = undo ? eh_open(&s) : -1;
    einit(&s, cx_fmt("%s[__cx_i%d]", arr, u), el, init);
    if (lp >= 0)
        eh_close(&s, lp, cx_fmt("while (__cx_i%d > 0) { __cx_i%d--; %s }",
                                u, u, undo));
    return s.p ? s.p : "";
}

static void einit(struct sb *b, const char *dest, struct cty *t,
                  struct cexpr *init)
{
    if (!init)
        return;
    if (ct_is_ref(t)) {
        sb_printf(b, "%s = %s; ", dest, ev(init));
        return;
    }
    switch (init->k) {
    case E_CALL: case E_ICALL: case E_PMCALL:
        if (call_sret(init)) {
            sb_printf(b, "%s; ", call_text(init, dest));
            return;
        }
        break;
    case E_CONSTRUCT:
        if (init->t->k == CT_ARRAY && !init->fn && init->na == 1) {
            sb_printf(b, "__builtin_memcpy(&(%s), %s, %ldUL); ", dest,
                      eaddr(init->a[0]), ct_size(init->t));
            return;
        }
        if (init->t->k == CT_ARRAY) {
            long n = 1;
            struct cty *el = init->t;
            while (el->k == CT_ARRAY) {
                n *= el->n;
                el = el->to;
            }
            int u = cx_uid();
            char *body = elem_init(cx_fmt("__cx_a%d", u), u, el, init->init);
            sb_printf(b, "{ %s = (%s)&(%s); for (unsigned long __cx_i%d = 0; "
                         "__cx_i%d < %ldUL; __cx_i%d++) { %s} } ",
                      cdecl(ct_ptr(el), cx_fmt("__cx_a%d", u)),
                      ctype(ct_ptr(el)), dest, u, u, n, u, body);
            return;
        }
        {
            /* a base subobject, or a class whose tail padding a derived
             * class may use: only its data is written */
            struct cclass *ic = init->t->cls;
            long bytes = init->baseobj ? ic->nvsize
                         : ic->dsize < ic->size ? ic->dsize : 0;
            if (init->zero && bytes)
                sb_printf(b, "__builtin_memset(&(%s), 0, %ldUL); ", dest,
                          bytes);
            else if (init->zero)
                sb_printf(b, "__builtin_memset(&(%s), 0, sizeof(%s)); ",
                          dest, dest);
            if (init->fn) {
                need_fn(init->fn);
                sb_printf(b, "%s(&(%s)%s%s%s); ",
                          fn_name(init->fn, init->baseobj ? 2 : 1), dest,
                          vtt_arg(init->fn, init->baseobj),
                          init->na ? ", " : "",
                          args_text(init->a, init->na, 0));
            } else if (init->na == 1 && bytes) {
                sb_printf(b, "__builtin_memcpy(&(%s), %s, %ldUL); ", dest,
                          eaddr(init->a[0]), bytes);
            } else if (init->na == 1) {
                sb_printf(b, "%s = %s; ", dest, ev(init->a[0]));
            }
        }
        return;
    case E_INITLIST: {
        struct cty *lt = init->t;
        int zero = 0;
        for (int i = 0; i < init->na; i++)
            if (!init->a[i])
                zero = 1;
        if (lt->k == CT_CLASS && lt->cls->is_union)
            zero = 1;
        if (zero)
            sb_printf(b, "__builtin_memset(&(%s), 0, sizeof(%s)); ", dest,
                      dest);
        for (int i = 0; i < init->na; i++) {
            if (!init->a[i])
                continue;
            if (lt->k == CT_ARRAY) {
                einit(b, cx_fmt("(%s)[%d]", dest, i), lt->to, init->a[i]);
            } else {
                struct cfield *fl = lt->cls->fields[i];
                einit(b, cx_fmt("(%s).%s", dest, field_cname(fl)), fl->type,
                      init->a[i]);
            }
        }
        return;
    }
    case E_STR: {
        if (t->k != CT_ARRAY)
            break;                  /* a pointer to the literal */
        long n = t->n >= 0 ? t->n : init->slen;
        long k = init->slen < n ? init->slen : n;
        sb_printf(b, "__builtin_memcpy(%s, %s, %ldUL); ", dest, str_lit(init),
                  k * init->swidth);
        if (n > k)
            sb_printf(b, "__builtin_memset((char *)%s + %ldUL, 0, %ldUL); ",
                      dest, k * init->swidth, (n - k) * init->swidth);
        return;
    }
    case E_COND:
        if (t->k == CT_CLASS) {
            struct sb x = { 0, 0, 0 }, y = { 0, 0, 0 };
            char *c = ev(init->a[0]);
            fx->cond++;
            einit(&x, dest, t, init->a[1]);
            einit(&y, dest, t, init->a[2]);
            fx->cond--;
            sb_printf(b, "if (%s) { %s} else { %s} ", c, sb_str(&x),
                      sb_str(&y));
            return;
        }
        break;
    case E_COMMA:
        if (t->k == CT_CLASS) {
            sb_printf(b, "(void)%s; ", ev(init->a[0]));
            einit(b, dest, t, init->a[1]);
            return;
        }
        break;
    default:
        break;
    }
    sb_printf(b, "%s = %s; ", dest, ev(init));
}

/* ---- constant initializers (for static storage) ---- */

static int addr_const(struct cexpr *e)
{
    switch (e->k) {
    case E_VAR:
        return (!e->var->is_local || e->var->is_static) &&
               !ct_is_ref(e->var->type);
    case E_STR:
        return 1;
    case E_MEMBER:
        return !ct_is_ref(e->field->type) && e->a[0]->k != E_DEREF &&
               addr_const(e->a[0]);
    case E_DEREF: {
        struct cexpr *p = e->a[0];
        if (p->k == E_BINARY && p->op == TOK_PLUS) {
            long v;
            return expr_fold(p->a[1], &v) && p->a[0]->t->k == CT_PTR &&
                   (p->a[0]->k == E_VAR || p->a[0]->k == E_STR ||
                    p->a[0]->k == E_MEMBER) && addr_const(p->a[0]);
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int c_const(struct cexpr *e)
{
    long v;
    if (!e)
        return 1;
    if (e->t && ct_is_integer(e->t) && expr_fold(e, &v))
        return 1;
    switch (e->k) {
    case E_INT: case E_FLT: case E_STR: case E_NULLPTR:
        return 1;
    case E_FUNC:
        return 1;
    case E_VAR:
        /* an array decayed to its address */
        return e->t->k == CT_PTR && addr_const(e);
    case E_MEMBER:
        return e->t->k == CT_PTR && addr_const(e);
    case E_ADDR:
        return addr_const(e->a[0]) || e->a[0]->k == E_FUNC;
    case E_MEMPTR:
        return 1;
    case E_CAST:
        if (e->lvcast)
            return 0;
        if (e->t->k == CT_MPTR)
            return e->a[0]->k != E_CALL && c_const(e->a[0]);
        if (e->t->k == CT_PTR || e->t->k == CT_BOOL)
            return c_const(e->a[0]) &&
                   (e->a[0]->t->k == CT_PTR || e->a[0]->k == E_NULLPTR ||
                    e->a[0]->k == E_INT || e->a[0]->k == E_FUNC ||
                    e->a[0]->k == E_ADDR);
        return ct_is_arith(e->t) && ct_is_arith(e->a[0]->t) &&
               c_const(e->a[0]);
    case E_UNARY:
        return ct_is_arith(e->t) && c_const(e->a[0]);
    case E_BINARY:
        if (e->t->k == CT_PTR)
            return c_const(e->a[0]) && expr_fold(e->a[1], &v);
        return ct_is_arith(e->a[0]->t) && ct_is_arith(e->a[1]->t) &&
               c_const(e->a[0]) && c_const(e->a[1]);
    case E_INITLIST:
        for (int i = 0; i < e->na; i++)
            if (!c_const(e->a[i]))
                return 0;
        return 1;
    case E_CONSTRUCT:
        return !e->fn && !e->na && e->t->k == CT_CLASS;
    default:
        return 0;
    }
}

/* A C initializer for t from the constant (c_const) e. */
static char *cinit_text(struct cty *t, struct cexpr *e)
{
    if (!e)
        return (t->k == CT_CLASS || t->k == CT_ARRAY) ? "{0}" : "0";
    if (e->k == E_CONSTRUCT)
        return "{0}";
    if (e->k == E_MEMPTR && e->fn) {
        need_fn(e->fn);
        return cx_fmt("{ (void *)%s, 0 }", fn_name(e->fn, 1));
    }
    if (e->k == E_CAST && ct_is_pmf(e->t) && e->a[0]->t->k != CT_MPTR)
        return "{ 0, 0 }";
    if (e->k == E_INITLIST) {
        struct sb b = { 0, 0, 0 };
        sb_put(&b, "{ ");
        int n = e->na;
        if (t->k == CT_CLASS && t->cls->is_union && n > 1)
            n = 1;
        int any = 0;
        for (int i = 0; i < n; i++) {
            struct cty *et;
            if (t->k == CT_ARRAY) {
                et = t->to;
            } else {
                struct cfield *fl = t->cls->fields[i];
                if (!fl->name)
                    continue;               /* unnamed bit-fields take none */
                et = fl->type;
            }
            sb_printf(&b, "%s%s", any ? ", " : "", cinit_text(et, e->a[i]));
            any = 1;
        }
        if (!any)
            sb_put(&b, "0");
        sb_put(&b, " }");
        return b.p;
    }
    struct fx f, *save;
    fx_begin(&f, &save);
    char *v = ev(e);
    fx_end(save);
    return v;
}

/* ---- statements ---- */

/* What must run when a scope is left: a destructor call (or a handler's
 * __cxa_end_catch), at every exit — and, with exceptions, in a region
 * opened as it was pushed, whose landing pad runs it when a call in the
 * rest of the scope throws. An eh_only one (a constructor's finished
 * members) runs only then. */
struct clean {
    char *text;
    struct cstmt *blk;
    int lp;                   /* its region's landing pad (-1: none) */
    int eh_only;
};

static struct clean *cleans;
static int ncleans, capcleans;
static int brk_mark[256], cont_mark[256];
static int nbrk, ncont;
static int line_now;
static const char *file_now;

static void push_clean(struct sb *b, char *text, struct cstmt *blk,
                       int eh_only)
{
    if (ncleans == capcleans) {
        capcleans = capcleans ? capcleans * 2 : 16;
        cleans = xrealloc(cleans, (size_t)capcleans * sizeof *cleans);
    }
    cleans[ncleans].text = text;
    cleans[ncleans].blk = blk;
    cleans[ncleans].lp = eh_on ? eh_open(b) : -1;
    cleans[ncleans].eh_only = eh_only;
    ncleans++;
}

/* Leaving the scopes above mark by a jump: their destructors. */
static void run_cleans(struct sb *b, int mark)
{
    for (int i = ncleans - 1; i >= mark; i--)
        if (!cleans[i].eh_only)
            sb_printf(b, "%s\n", cleans[i].text);
}

/* The end of the scopes above mark: each region closed (its pad runs its
 * cleanup and hands on), and the cleanup run. */
static void close_cleans(struct sb *b, int mark)
{
    for (int i = ncleans - 1; i >= mark; i--) {
        if (cleans[i].lp >= 0)
            eh_close(b, cleans[i].lp, cleans[i].text);
        if (!cleans[i].eh_only)
            sb_printf(b, "%s\n", cleans[i].text);
    }
    ncleans = mark;
}

static void line_marker(struct sb *b, int line, const char *file)
{
    if (!file || !line || (line == line_now && file == file_now))
        return;
    if (b->len && b->p[b->len - 1] != '\n')
        sb_put(b, "\n");
    sb_printf(b, "# %d \"%s\"\n", line, file);
    line_now = line;
    file_now = file;
}

/* the local static's names: C statics in their function, or — in an
 * inline function, where every unit's copy must share one object — weak
 * globals under their Itanium names */
static void local_static_names(struct cvar *v, char **guard)
{
    if (cur_fn->is_inline && !fn_internal(cur_fn)) {
        if (strncmp(v->cname, "_ZZ", 3) != 0)
            v->cname = mangle_local_static(v, cur_fn, v->disc);
        *guard = cx_fmt("_ZGV%s", v->cname + 2);
    } else {
        *guard = cx_fmt("__cx_guard%d_%s", cx_uid(), v->cname);
    }
}

static int need_guard_init(struct cvar *v)
{
    struct cty *t = v->type;
    if (ct_is_ref(t))
        return !c_const(v->init);
    if (t->k == CT_CLASS || t->k == CT_ARRAY)
        return v->ctor && !c_const(v->ctor);
    return v->init && !c_const(v->init);
}

static void emit_local_static(struct sb *b, struct cstmt *s)
{
    struct cvar *v = s->var;
    struct cty *t = v->type;
    char *guard;
    local_static_names(v, &guard);
    int shared = strncmp(v->cname, "_ZZ", 3) == 0;
    struct cexpr *ini = ct_is_ref(t) || !(t->k == CT_CLASS ||
                                          t->k == CT_ARRAY) ? v->init
                                                            : v->ctor;
    int dyn = need_guard_init(v);
    char *dtor = destroy_text(cx_fmt("&%s", v->cname), t);
    const char *init_c = !dyn && ini ? cx_fmt(" = %s", cinit_text(t, ini))
                                     : "";
    if (shared) {
        sb_printf(&out_vars, "__attribute__((weak)) %s%s;\n",
                  cdecl(t, v->cname), init_c);
        if (dyn || dtor)
            sb_printf(&out_vars, "__attribute__((weak)) long long %s;\n",
                      guard);
    } else {
        sb_printf(b, "static %s%s;\n", cdecl(t, v->cname), init_c);
        if (dyn || dtor)
            sb_printf(b, "static long long %s;\n", guard);
    }
    if (!dyn && !dtor)
        return;
    need_guard = 1;
    sb_printf(b, "if (*(volatile char *)&%s == 0 && __cxa_guard_acquire(&%s)) "
                 "{\n", guard, guard);
    /* an initializer that throws leaves it to be tried again */
    int lp = dyn && eh_on ? eh_open(b) : -1;
    if (dyn)
        stmt_init(b, v->cname, t, ini);
    if (lp >= 0)
        eh_close(b, lp, cx_fmt("__cxa_guard_abort(&%s);", guard));
    if (dtor) {
        struct cfunc *d = class_dtor((t->k == CT_ARRAY ? t->to : t)->cls);
        if (t->k == CT_CLASS && d) {
            need_atexit = 1;
            sb_printf(b, "__cxa_atexit((void (*)(void *))%s, &%s, "
                         "&__dso_handle);\n", fn_name(d, 1), v->cname);
        }
    }
    sb_printf(b, "__cxa_guard_release(&%s);\n}\n", guard);
}

/* Can this (automatic) object take a C initializer: a list of values
 * with no constructors to run? */
static int c_list_ok(struct cexpr *e)
{
    if (!e)
        return 1;
    if (e->k == E_INITLIST) {
        if (e->t->k == CT_CLASS && e->t->cls->is_union)
            return 0;
        for (int i = 0; i < e->na; i++)
            if (!c_list_ok(e->a[i]))
                return 0;
        return 1;
    }
    if (e->k == E_STR)
        return 1;
    if (e->k == E_CONSTRUCT)
        return !e->fn && !e->na && e->t->k == CT_CLASS;
    return e->t && e->t->k != CT_CLASS && e->t->k != CT_ARRAY &&
           c_const(e);
}

static void emit_decl(struct sb *b, struct cstmt *s)
{
    struct cvar *v = s->var;
    struct cty *t = v->type;
    line_marker(b, s->line, s->file);
    if (v->is_static) {
        emit_local_static(b, s);
        return;
    }
    const char *al = v->align_attr ? cx_fmt(" __attribute__((aligned(%ld)))",
                                            v->align_attr) : "";
    if (ct_is_ref(t)) {
        struct cexpr *p = v->init;
        if (p && p->k == E_ADDR && p->a[0]->k == E_TEMP) {
            /* a temporary bound to a local reference lives as long */
            struct cexpr *tmp = p->a[0];
            char *name = cx_fmt("__cx_e%d", cx_uid());
            sb_printf(b, "%s;\n", cdecl(ct_unqual(tmp->t), name));
            stmt_init(b, name, ct_unqual(tmp->t), tmp->a[0]);
            char *d = destroy_text(cx_fmt("&%s", name), ct_unqual(tmp->t));
            if (d)
                push_clean(b, d, s->blk, 0);
            sb_printf(b, "%s = &%s;\n", cdecl(t, v->cname), name);
            return;
        }
        sb_printf(b, "%s%s = %s;\n", cdecl(t, v->cname), al,
                  p ? full_value(p) : "0");
        return;
    }
    if (v->nrvo) {
        /* the named return value: built in the caller's slot, never
         * destroyed here */
        v->cname = "__cx_sret";
        if (v->ctor)
            stmt_init(b, "(*__cx_sret)", t, v->ctor);
        return;
    }
    if (t->k == CT_CLASS || t->k == CT_ARRAY) {
        struct cexpr *c = v->ctor;
        if (c && (c->k == E_INITLIST || c->k == E_STR) && c_list_ok(c)) {
            sb_printf(b, "%s%s = %s;\n", cdecl(t, v->cname), al,
                      c->k == E_STR ? str_lit(c) : cinit_text(t, c));
        } else {
            sb_printf(b, "%s%s;\n", cdecl(t, v->cname), al);
            if (c)
                stmt_init(b, v->cname, t, c);
        }
        if (s->dtor) {
            char *d = destroy_text(cx_fmt("&%s", v->cname), t);
            if (d)
                push_clean(b, d, s->blk, 0);
        }
        return;
    }
    if (v->init)
        sb_printf(b, "%s%s = %s;\n", cdecl(t, v->cname), al,
                  full_value(v->init));
    else
        sb_printf(b, "%s%s;\n", cdecl(t, v->cname), al);
}

static int is_ancestor(struct cstmt *a, struct cstmt *blk)
{
    for (; blk; blk = blk->blk)
        if (blk == a)
            return 1;
    return 0;
}

static struct cstmt *find_label(struct cstmt *s, const char *name)
{
    for (; s; s = s->next) {
        if (s->k == S_LABEL && strcmp(s->label, name) == 0)
            return s;
        struct cstmt *r = NULL;
        if (s->body && (r = find_label(s->body, name)))
            return r;
        if (s->els && (r = find_label(s->els, name)))
            return r;
    }
    return NULL;
}

/* The declarations of an if/while/for/switch's init, then the statement,
 * then their destruction. */
static int emit_inits(struct sb *b, struct cstmt *init)
{
    int mark = ncleans;
    for (struct cstmt *d = init; d; d = d->more) {
        if (d->k == S_DECL)
            emit_decl(b, d);
        else
            emit_stmt(b, d);
    }
    return mark;
}

/* The typeinfo a handler for t names: the type caught, without its
 * reference and top-level qualifiers (NULL t: catch (...), 0). */
static const char *catch_ti(struct cty *t)
{
    if (!t)
        return "0";
    if (ct_is_ref(t))
        t = t->to;
    t = ct_unqual(ct_decay(t));
    return typeinfo_sym(t);
}

/* try { body } catch ...: body in a region whose landing pad picks the
 * handler by the selector — the type table index of the one the
 * personality routine matched — or hands on. A handler is a block that
 * begins the catch and whose cleanup ends it (on every way out). */
static void emit_handlers(struct sb *b, struct cstmt *s, int lp,
                          int rethrow);

static void emit_try(struct sb *b, struct cstmt *s)
{
    int lp = eh_open(b);
    emit_stmt(b, s->body);
    emit_handlers(b, s, lp, 0);
}

/* Close a try's region lp with its landing pad: the handlers (each
 * rethrowing at its end when `rethrow`: a constructor's or destructor's
 * function-try-block), else hand on. */
static void emit_handlers(struct sb *b, struct cstmt *s, int lp,
                          int rethrow)
{
    neh_lp--;
    sb_put(b, "} __builtin_eh_landing (__cx_exc, __cx_sel");
    for (int i = 0; i < s->nhandlers; i++)
        sb_printf(b, ", %s", catch_ti(s->handlers[i].type));
    sb_printf(b, ") {\n__cx_lp%d:;\n", lp);
    for (int i = 0; i < s->nhandlers; i++) {
        struct chandler *h = &s->handlers[i];
        sb_printf(b, "%sif (__cx_sel == __builtin_eh_typeid(%s)) {\n",
                  i ? "else " : "", catch_ti(h->type));
        sb_put(b, "void *__cx_c = ((void *(*)(void *))__cxa_begin_catch)"
                  "(__cx_exc);\n");
        int mark = ncleans;
        push_clean(b, "((void (*)(void))__cxa_end_catch)();", h->body, 0);
        emit_block_items(b, h->body->body);
        if (rethrow)
            sb_put(b, "((void (*)(void))__cxa_rethrow)(); "
                      "__builtin_unreachable();\n");
        close_cleans(b, mark);
        sb_put(b, "}\n");
    }
    sb_put(b, "else {\n");
    eh_chain(b);
    sb_put(b, "}\n}\n");
}

static void emit_stmt(struct sb *b, struct cstmt *s)
{
    if (s->k != S_BLOCK && s->k != S_DECL)
        line_marker(b, s->line, s->file);
    switch (s->k) {
    case S_NULL:
        sb_put(b, ";\n");
        return;
    case S_EXPR:
        full_stmt(b, s->e);
        return;
    case S_DECL:
        emit_decl(b, s);
        return;
    case S_BLOCK: {
        sb_put(b, "{\n");
        int mark = ncleans;
        emit_block_items(b, s->body);
        close_cleans(b, mark);
        sb_put(b, "}\n");
        return;
    }
    case S_IF: {
        sb_put(b, "{\n");
        int mark = emit_inits(b, s->init);
        sb_printf(b, "if (%s) ", full_value(s->e));
        emit_stmt(b, s->body);
        if (s->els) {
            sb_put(b, "else ");
            emit_stmt(b, s->els);
        }
        close_cleans(b, mark);
        sb_put(b, "}\n");
        return;
    }
    case S_WHILE:
        if (s->init) {
            /* the condition's variable is made anew each iteration */
            sb_put(b, "for (;;) {\n");
            int mark = emit_inits(b, s->init);
            sb_printf(b, "if (!(%s)) { ", full_value(s->e));
            run_cleans(b, mark);
            sb_put(b, "break; }\n");
            brk_mark[nbrk++] = mark;
            cont_mark[ncont++] = mark;
            emit_stmt(b, s->body);
            nbrk--;
            ncont--;
            close_cleans(b, mark);
            sb_put(b, "}\n");
            return;
        }
        sb_printf(b, "while (%s) ", full_value(s->e));
        brk_mark[nbrk++] = ncleans;
        cont_mark[ncont++] = ncleans;
        emit_stmt(b, s->body);
        nbrk--;
        ncont--;
        return;
    case S_DO:
        sb_put(b, "do ");
        brk_mark[nbrk++] = ncleans;
        cont_mark[ncont++] = ncleans;
        emit_stmt(b, s->body);
        nbrk--;
        ncont--;
        sb_printf(b, "while (%s);\n", full_value(s->e));
        return;
    case S_FOR: {
        sb_put(b, "{\n");
        int mark = emit_inits(b, s->init);
        sb_printf(b, "for (; %s; %s) ", s->e ? full_value(s->e) : "",
                  s->e2 ? full_value(s->e2) : "");
        brk_mark[nbrk++] = ncleans;
        cont_mark[ncont++] = ncleans;
        emit_stmt(b, s->body);
        nbrk--;
        ncont--;
        close_cleans(b, mark);
        sb_put(b, "}\n");
        return;
    }
    case S_SWITCH: {
        sb_put(b, "{\n");
        int mark = emit_inits(b, s->init);
        sb_printf(b, "switch (%s) ", full_value(s->e));
        brk_mark[nbrk++] = ncleans;
        emit_stmt(b, s->body);
        nbrk--;
        close_cleans(b, mark);
        sb_put(b, "}\n");
        return;
    }
    case S_CASE:
        if (s->is_range)
            sb_printf(b, "case %ld ... %ld:\n", s->cval, s->cval2);
        else
            sb_printf(b, "case %s:\n", int_lit(s->cval, ct_basic(CT_LONG)));
        emit_stmt(b, s->body);
        return;
    case S_DEFAULT:
        sb_put(b, "default:\n");
        emit_stmt(b, s->body);
        return;
    /* a jump runs the destructors of the scopes it leaves: its own braces
     * keep them with it under a brace-less if */
    case S_BREAK:
        sb_put(b, "{ ");
        if (nbrk)
            run_cleans(b, brk_mark[nbrk - 1]);
        sb_put(b, "break; }\n");
        return;
    case S_CONTINUE:
        sb_put(b, "{ ");
        if (ncont)
            run_cleans(b, cont_mark[ncont - 1]);
        sb_put(b, "continue; }\n");
        return;
    case S_RETURN: {
        struct cty *rt = cur_fn->type->to;
        if (cur_fn->is_dtor || !s->e || rt->k == CT_VOID) {
            sb_put(b, "{ ");
            if (s->e)
                full_stmt(b, s->e);
            run_cleans(b, 0);
            sb_put(b, cur_fn->is_dtor ? "goto __cx_dtor_end; }\n"
                                      : "return; }\n");
            return;
        }
        if (class_indirect(rt)) {
            /* into the caller's slot; the named return value is there */
            sb_put(b, "{ ");
            if (!s->ret_var || !s->ret_var->nrvo)
                stmt_init(b, "(*__cx_sret)", ct_unqual(rt), s->e);
            run_cleans(b, 0);
            sb_put(b, "return __cx_sret; }\n");
            return;
        }
        if (!ncleans) {
            if (rt->k == CT_CLASS && s->e->k != E_CALL) {
                sb_printf(b, "{ %s; ", cdecl(ct_unqual(rt), "__cx_ret"));
                stmt_init(b, "__cx_ret", ct_unqual(rt), s->e);
                sb_put(b, "return __cx_ret; }\n");
                return;
            }
            sb_printf(b, "return %s;\n", full_value(s->e));
            return;
        }
        struct cty *ct = ct_unqual(rt);
        sb_printf(b, "{ %s; ", cdecl(ct, "__cx_ret"));
        stmt_init(b, "__cx_ret", ct_is_ref(rt) ? rt : ct, s->e);
        run_cleans(b, 0);
        sb_put(b, "return __cx_ret; }\n");
        return;
    }
    case S_GOTO: {
        struct cstmt *l = find_label(cur_fn->body, s->label);
        if (!l)
            cx_error(NULL, "label '%s' used but not defined", s->label);
        sb_put(b, "{ ");
        for (int i = ncleans - 1; i >= 0; i--)
            if (!cleans[i].eh_only && !is_ancestor(cleans[i].blk, l->blk))
                sb_printf(b, "%s\n", cleans[i].text);
        sb_printf(b, "goto %s; }\n", s->label);
        return;
    }
    case S_LABEL:
        sb_printf(b, "%s:\n", s->label);
        emit_stmt(b, s->body);
        return;
    case S_ASM:
        sb_printf(b, "%s\n", s->asm_text);
        return;
    case S_TRY:
        emit_try(b, s);
        return;
    }
}

static void emit_block_items(struct sb *b, struct cstmt *first)
{
    for (struct cstmt *s = first; s; s = s->next)
        emit_stmt(b, s);
}

/* ---- functions ---- */

/* What func_header adds after `this`: 1 the VTT (a base-object
 * constructor or destructor of a class with virtual bases), 2 that and
 * whether the object is complete (their shared body). */
static int hdr_vtt;

static char *func_header(struct cfunc *f, const char *name, int named)
{
    struct sb p = { 0, 0, 0 };
    int any = 0;
    int vtt = hdr_vtt;
    hdr_vtt = 0;
    int sret = class_indirect(f->type->to);
    if (sret) {
        /* the return slot comes first, before `this` (Itanium; x8 on
         * aarch64 through embcc_sret) */
        sb_printf(&p, "__attribute__((embcc_sret)) %s",
                  cdecl(ct_ptr(f->type->to), named ? "__cx_sret" : ""));
        any = 1;
    }
    if (f->cls && !f->is_static) {
        sb_printf(&p, "%s%s", any ? ", " : "",
                  cdecl(ct_ptr(ct_class(f->cls)), named ? "this" : ""));
        any = 1;
    }
    if (vtt)
        sb_printf(&p, ", void **%s", named ? "__cx_vtt" : "");
    if (vtt == 2)
        sb_printf(&p, ", int %s", named ? "__cx_complete" : "");
    struct cty *ft = f->type;
    for (int i = 0; i < ft->np; i++) {
        const char *pn = "";
        if (named)
            pn = f->params && f->params[i] ? f->params[i]->cname
                                           : cx_fmt("__cx_p%d", i);
        struct cty *pt = ft->params[i];
        sb_printf(&p, "%s%s", any ? ", " : "",
                  cdecl(class_indirect(pt) ? ct_ptr(pt) : pt, pn));
        any = 1;
    }
    if (ft->variadic)
        sb_put(&p, any ? ", ..." : "...");
    else if (!any)
        sb_put(&p, "void");
    struct cty *rt = sret ? ct_ptr(ft->to) : ft->to;
    if (rt->k == CT_PTR && (rt->to->k == CT_FUNC || rt->to->k == CT_ARRAY)) {
        /* C's parser takes a function returning a pointer to a function
         * (or an array) through a typedef */
        const char *td = cx_fmt("__cx_rt%d", cx_uid());
        sb_printf(&out_rettypes, "typedef %s;\n", cdecl(rt, td));
        return cx_fmt("%s %s(%s)", td, name, sb_str(&p));
    }
    return cdecl(sret ? ct_ptr(ft->to) : ft->to,
                 cx_fmt("%s(%s)", name, sb_str(&p)));
}

static const char *fn_storage(struct cfunc *f)
{
    if (fn_internal(f))
        return "static ";
    if (f->is_inline || f->is_implicit || f->weak)
        return "__attribute__((weak)) ";
    return "";
}

static void emit_prototype(struct cfunc *f)
{
    /* weak goes on definitions only: a weak declaration would let a
     * missing body link as address 0 */
    const char *st = fn_internal(f) ? "static " : "";
    /* one that cannot throw: its calls need no landing pads */
    const char *sec = eh_on && func_nothrow(f) ? "__attribute__((nothrow)) "
                                               : "";
    if (f->is_ctor || f->is_dtor) {
        sb_printf(&out_decls, "%s%s%s;\n", st, sec,
                  func_header(f, fn_name(f, 1), 0));
        hdr_vtt = f->cls->nvbases > 0;
        sb_printf(&out_decls, "%s%s%s;\n", st, sec,
                  func_header(f, fn_name(f, 2), 0));
        if (f->is_dtor && f->is_virtual)
            sb_printf(&out_decls, "%s%s%s;\n", st, sec,
                      func_header(f, fn_name(f, 0), 0));
        return;
    }
    sb_printf(&out_decls, "%s%s%s;\n", st, sec,
              func_header(f, fn_name(f, 1), 0));
}

/* The i-th direct base subobject of *this, as a C lvalue. */
static char *base_lvalue(struct cclass *c, int i)
{
    struct cbase *b = &c->bases[i];
    return cx_fmt("(*(%s)((char *)this + %ldL))",
                  ctype(ct_ptr(ct_class(b->cls))), b->off);
}

/* Virtual base k of c (a complete object's), as a C lvalue. */
static char *vbase_lvalue(struct cclass *c, int k)
{
    return cx_fmt("(*(%s)((char *)this + %ldL))",
                  ctype(ct_ptr(ct_class(c->vbases[k].cls))), c->vbases[k].off);
}

/* Point every vptr of the object at c's vtables: after the bases are
 * built (they set their own), before members and the body. With virtual
 * bases the addresses come from `vtt` — the class's own VTT when the
 * object is complete, the sub-VTT its derived class passed when it is a
 * base — and a virtual base is found through the vptr stored first. */
static void store_vptrs(struct sb *b, struct cclass *c, const char *vtt)
{
    if (!c->dynamic)
        return;
    need_vtable(c);
    struct vstore *st;
    int n = class_vstores(c, &st);
    for (int i = 0; i < n; i++) {
        struct vstore *v = &st[i];
        const char *val = vtt && v->vtt >= 0
                          ? cx_fmt("%s[%d]", vtt, v->vtt)
                          : cx_fmt("(void **)%s + %d", class_sym(c, "_ZTV"),
                                   v->point);
        const char *at = v->virt
                         ? cx_fmt("(char *)this + *(long *)(*(char **)this + "
                                  "%ldL) + %ldL", v->vbindex, v->off)
                         : cx_fmt("(char *)this + %ldL", v->off);
        sb_printf(b, "*(void ***)(%s) = (void **)%s;\n", at, val);
    }
}

static int vbase_index(struct cclass *c, struct cclass *v)
{
    for (int k = 0; k < c->nvbases; k++)
        if (c->vbases[k].cls == v)
            return k;
    return -1;
}

/* The VTT argument of a base-object constructor or destructor call, when
 * its class has virtual bases: cur_vtt_base, set by the caller. */
static const char *cur_vtt_base;

static const char *vtt_arg(struct cfunc *f, int baseobj)
{
    if (!baseobj || !f->cls || !f->cls->nvbases || f->is_static ||
        !(f->is_ctor || f->is_dtor))
        return "";
    if (!cur_vtt_base)
        cx_error(NULL, "internal: base '%s' built without its VTT",
                 f->cls->name);
    return cx_fmt(", %s", cur_vtt_base);
}

static void emit_function(struct cfunc *f)
{
    if (f->emitted || !f->defined)
        return;
    f->emitted = 1;
    f->declared = 1;
    cur_fn = f;
    ncleans = 0;
    nbrk = ncont = 0;
    line_now = 0;
    fn_eh = 0;
    neh_lp = 0;
    struct sb b = { 0, 0, 0 };
    int special = f->is_ctor || f->is_dtor;
    struct cclass *c = f->cls;
    /* with virtual bases, the complete-object and base-object variants
     * share a body that takes the VTT and whether the object is complete */
    int vtt = special && c->nvbases > 0;
    const char *name = fn_name(f, special ? 2 : 1);
    const char *body = vtt ? cx_fmt("%s__impl", name) : name;
    const char *st = fn_storage(f);
    const char *sec = "";
    if (f->section)
        cx_warn(NULL, "section(\"%s\") on function '%s' is ignored",
                f->section, f->name);
    line_marker(&b, f->line, f->file);
    if (vtt)
        hdr_vtt = 2;
    sb_printf(&b, "%s%s%s\n{\n", vtt ? "static " : st, sec,
              func_header(f, body, 1));
    int body_at = b.len;          /* where __cx_exc/__cx_sel go, if used */
    /* noexcept (a destructor is, implicitly): an exception leaving it
     * calls std::terminate — a catch-all region around everything */
    int nothrow_lp = -1;
    if (eh_on && (f->type->nothrow || f->is_dtor))
        nothrow_lp = eh_open(&b);
    /* a function-try-block: the whole function (a constructor's bases and
     * members too) in the try's region */
    int try_lp = -1;
    if (eh_on && f->fn_try)
        try_lp = eh_open(&b);
    if (f->is_ctor) {
        if (!f->delegate && vtt && f->vbaseinit) {
            /* only the most derived class builds the virtual bases, in
             * their construction order */
            sb_put(&b, "if (__cx_complete) {\n");
            for (int k = 0; k < c->nvinit; k++) {
                int v = vbase_index(c, c->vinit[k]);
                if (v < 0 || !f->vbaseinit[v])
                    continue;
                int vv = class_vvtt(c, v);
                cur_vtt_base = vv >= 0 ? cx_fmt("__cx_vtt + %d", vv) : NULL;
                stmt_init(&b, vbase_lvalue(c, v), ct_class(c->vbases[v].cls),
                          f->vbaseinit[v]);
            }
            sb_put(&b, "}\n");
        }
        if (!f->delegate && f->baseinit)
            for (int i = 0; i < c->nbases; i++) {
                if (!f->baseinit[i] || c->bases[i].is_virtual)
                    continue;
                int sv = vtt ? class_subvtt(c, i) : -1;
                cur_vtt_base = sv >= 0 ? cx_fmt("__cx_vtt + %d", sv) : NULL;
                stmt_init(&b, base_lvalue(c, i), ct_class(c->bases[i].cls),
                          f->baseinit[i]);
                /* built: destroyed if the rest of the constructor throws */
                struct cfunc *bd = class_dtor(c->bases[i].cls);
                if (eh_on && bd) {
                    need_fn(bd);
                    push_clean(&b, cx_fmt("%s(&%s%s);", fn_name(bd, 2),
                                          base_lvalue(c, i),
                                          sv >= 0 ? cx_fmt(", __cx_vtt + %d",
                                                           sv) : ""),
                               NULL, 1);
                }
            }
        cur_vtt_base = NULL;
        if (!f->delegate)
            store_vptrs(&b, c, vtt ? "__cx_vtt" : NULL);
        if (f->delegate && vtt) {
            /* the target's complete or base-object variant, as this one */
            sb_put(&b, "if (__cx_complete) {\n");
            stmt_init(&b, "(*this)", ct_class(c), f->delegate);
            sb_put(&b, "} else {\n");
            int saved = f->delegate->baseobj;
            f->delegate->baseobj = 1;
            cur_vtt_base = "__cx_vtt";
            stmt_init(&b, "(*this)", ct_class(c), f->delegate);
            cur_vtt_base = NULL;
            f->delegate->baseobj = saved;
            sb_put(&b, "}\n");
        } else if (f->delegate) {
            stmt_init(&b, "(*this)", ct_class(c), f->delegate);
        } else if (f->meminit) {
            for (int i = 0; i < c->nfields; i++) {
                struct cfield *fl = c->fields[i];
                if (!f->meminit[i] || !fl->name)
                    continue;
                stmt_init(&b, cx_fmt("this->%s", field_cname(fl)), fl->type,
                          f->meminit[i]);
                char *d = eh_on && !ct_is_ref(fl->type)
                          ? destroy_text(cx_fmt("&this->%s", field_cname(fl)),
                                         fl->type) : NULL;
                if (d)           /* built: destroyed if the rest throws */
                    push_clean(&b, d, NULL, 1);
            }
        }
        if (f->delegate && eh_on) {
            /* the object is complete: its destructor, if the body throws */
            struct cfunc *d = class_dtor(c);
            if (d) {
                need_fn(d);
                push_clean(&b, cx_fmt("%s(this);", fn_name(d, 1)), NULL, 1);
            }
        }
    }
    if (f->is_dtor)           /* virtual calls in it reach this class */
        store_vptrs(&b, c, vtt ? "__cx_vtt" : NULL);
    if (f->body)
        emit_block_items(&b, f->body->body);
    close_cleans(&b, 0);
    if (f->is_dtor) {
        sb_put(&b, "goto __cx_dtor_end;\n__cx_dtor_end: ;\n");
        for (int i = c->nfields - 1; i >= 0; i--) {
            struct cfield *fl = c->fields[i];
            if (!fl->name || ct_is_ref(fl->type))
                continue;
            char *d = destroy_text(cx_fmt("&this->%s", field_cname(fl)),
                                   fl->type);
            if (d)
                sb_printf(&b, "%s\n", d);
        }
        for (int i = c->nbases - 1; i >= 0; i--) {
            struct cfunc *bd = class_dtor(c->bases[i].cls);
            if (!bd || c->bases[i].is_virtual)
                continue;
            need_fn(bd);
            int sv = vtt ? class_subvtt(c, i) : -1;
            sb_printf(&b, "%s(&%s%s);\n", fn_name(bd, 2), base_lvalue(c, i),
                      sv >= 0 ? cx_fmt(", __cx_vtt + %d", sv) : "");
        }
        if (vtt) {
            /* the most derived class's virtual bases, in reverse order */
            sb_put(&b, "if (__cx_complete) {\n");
            for (int k = c->nvinit - 1; k >= 0; k--) {
                int v = vbase_index(c, c->vinit[k]);
                struct cfunc *bd = class_dtor(c->vinit[k]);
                if (v < 0 || !bd)
                    continue;
                need_fn(bd);
                int vv = class_vvtt(c, v);
                sb_printf(&b, "%s(&%s%s);\n", fn_name(bd, 2),
                          vbase_lvalue(c, v),
                          vv >= 0 ? cx_fmt(", __cx_vtt + %d", vv) : "");
            }
            sb_put(&b, "}\n");
        }
    }
    if (try_lp >= 0)
        emit_handlers(&b, f->fn_try, try_lp, special);
    /* flowing off the end of a non-void function is undefined — and a
     * body ending in a throw does not (to C's eye) return */
    if (!special && f->type->to->k != CT_VOID &&
        !(!f->cls && !f->c_linkage && f->owner == cx_global &&
          strcmp(f->name, "main") == 0))
        sb_put(&b, "__builtin_unreachable();\n");
    if (nothrow_lp >= 0) {
        neh_lp--;
        sb_printf(&b, "} __builtin_eh_landing (__cx_exc, __cx_sel, 0) {\n"
                      "__cx_lp%d:;\n((void (*)(void *))__cxa_call_terminate)"
                      "(__cx_exc); __builtin_unreachable();\n}\n",
                  nothrow_lp);
    }
    if (!f->cls && !f->c_linkage && f->owner == cx_global &&
        strcmp(f->name, "main") == 0)
        sb_put(&b, "return 0;\n");
    sb_put(&b, "}\n");
    if (fn_eh) {
        /* the landing pads' exception pointer and selector */
        struct sb nb = { 0, 0, 0 };
        sb_printf(&nb, "%.*s", body_at, b.p);
        sb_put(&nb, "void *__cx_exc; long __cx_sel;\n");
        sb_put(&nb, b.p + body_at);
        b = nb;
    }
    if (special && vtt) {
        /* C2/D2 take the VTT; C1/D1 pass the class's own */
        struct sb args = { 0, 0, 0 };
        for (int i = 0; i < f->type->np; i++)
            sb_printf(&args, ", %s", f->params && f->params[i]
                      ? f->params[i]->cname : cx_fmt("__cx_p%d", i));
        hdr_vtt = 1;
        sb_printf(&b, "%s%s%s\n{\n%s(this, __cx_vtt, 0%s);\n}\n", st, sec,
                  func_header(f, name, 1), body, sb_str(&args));
        need_vtable(c);
        sb_printf(&b, "%s%s%s\n{\n%s(this, %s, 1%s);\n}\n", st, sec,
                  func_header(f, fn_name(f, 1), 1), body,
                  class_sym(c, "_ZTT"), sb_str(&args));
    } else if (special) {
        /* the complete-object variant: the same, no virtual bases */
        struct sb args = { 0, 0, 0 };
        sb_put(&args, "this");
        for (int i = 0; i < f->type->np; i++)
            sb_printf(&args, ", %s", f->params && f->params[i]
                      ? f->params[i]->cname : cx_fmt("__cx_p%d", i));
        sb_printf(&b, "%s%s%s\n{\n%s(%s);\n}\n", st, sec,
                  func_header(f, fn_name(f, 1), 1), name, sb_str(&args));
    }
    if (special) {
        if (f->is_dtor && f->is_virtual) {
            /* the deleting destructor: destroy, then free with the
             * class's size */
            struct cexpr *vp = ex_new(E_NULLPTR, ct_ptr(ct_basic(CT_VOID)),
                                      VC_PRVALUE);
            struct cexpr *sz = ex_int(c->size, ct_size_t());
            struct cexpr *oa[2] = { vp, sz };
            struct cexpr *call = call_delete_op(c, oa);
            need_fn(call->fn);
            sb_printf(&b, "%s%s\n{\n%s(this);\n%s(this%s);\n}\n", st,
                      func_header(f, fn_name(f, 0), 1), fn_name(f, 1),
                      fn_name(call->fn, 1), call->fn->type->np == 2
                      ? cx_fmt(", %ldUL", c->size) : "");
        }
    }
    sb_put(&out_code, sb_str(&b));
    cur_fn = NULL;
}

/* ---- variables ---- */

static void emit_gvar(struct cvar *v)
{
    v->emitted = 1;
    v->declared = 1;
    struct cty *t = v->type;
    const char *st = v->is_static ? "static "
                     : (v->is_inline || v->weak) ? "__attribute__((weak)) "
                     : "";
    struct sb attrs = { 0, 0, 0 };
    if (v->section)
        sb_printf(&attrs, " __attribute__((section(\"%s\")))", v->section);
    if (v->align_attr)
        sb_printf(&attrs, " __attribute__((aligned(%ld)))", v->align_attr);
    struct cexpr *ini = ct_is_ref(t) || !(t->k == CT_CLASS ||
                                          t->k == CT_ARRAY) ? v->init
                                                            : v->ctor;
    int dyn = ini && !c_const(ini);
    if (!dyn && ini)
        sb_printf(&out_vars, "%s%s%s = %s;\n", st, cdecl(t, v->cname),
                  sb_str(&attrs), ini->k == E_STR && t->k == CT_ARRAY
                  ? str_lit(ini) : cinit_text(t, ini));
    else
        sb_printf(&out_vars, "%s%s%s;\n", st, cdecl(t, v->cname),
                  sb_str(&attrs));
    if (dyn) {
        line_marker(&out_init, v->line, v->file);
        stmt_init(&out_init, v->cname, t, ini);
    }
    if (t->k == CT_CLASS) {
        struct cfunc *d = class_dtor(t->cls);
        if (d) {
            need_fn(d);
            need_atexit = 1;
            sb_printf(&out_init, "__cxa_atexit((void (*)(void *))%s, &%s, "
                                 "&__dso_handle);\n", fn_name(d, 1),
                      v->cname);
        }
    } else if (t->k == CT_ARRAY) {
        char *d = destroy_text(cx_fmt("&%s", v->cname), t);
        if (d)
            cx_error(NULL, "a namespace-scope array of objects with "
                           "destructors is not supported yet");
    }
}

/* Does initializing or destroying v run code? Then it exists whether or
 * not anything names it (a static object kept for its constructor). */
static int gvar_has_effects(struct cvar *v)
{
    struct cty *t = v->type;
    struct cexpr *ini = ct_is_ref(t) || !(t->k == CT_CLASS ||
                                          t->k == CT_ARRAY) ? v->init
                                                            : v->ctor;
    if (ini && !c_const(ini))
        return 1;
    struct cty *e = t;
    while (e->k == CT_ARRAY)
        e = e->to;
    return e->k == CT_CLASS && class_dtor(e->cls) != NULL;
}

static int gvar_needed(struct cvar *v)
{
    if (!v->defined || v->emitted)
        return 0;
    if (v->is_inline)
        return v->refd;
    if (v->is_static)
        return v->refd || gvar_has_effects(v);
    return 1;
}

/* ---- classes with bases or a vptr ---- */

struct item {
    long off, size;
    char *decl;
};

static int item_cmp(const void *a, const void *b)
{
    const struct item *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

/* The Itanium layout, spelled out: the vptr, each base's bytes, the
 * fields, at the offsets class.c chose — packed, with explicit padding,
 * so C puts nothing anywhere else. */
static void emit_explicit_struct(struct sb *out, struct cclass *c)
{
    struct item *it = xmalloc((size_t)(c->nfields + c->nbases +
                                       c->nvbases + 2) * sizeof *it);
    int n = 0;
    if (c->dynamic && !c->primary)
        it[n++] = (struct item){ 0, 8, "void *__cx_vptr" };
    for (int i = 0; i < c->nbases; i++) {
        struct cclass *b = c->bases[i].cls;
        if (b->empty || c->bases[i].is_virtual)
            continue;
        it[n++] = (struct item){ c->bases[i].off, b->nvsize,
                                 cx_fmt("char __cx_base%d[%ld]", i,
                                        b->nvsize) };
    }
    for (int i = 0; i < c->nvbases; i++) {
        struct cclass *b = c->vbases[i].cls;
        if (b->empty || c->vbases[i].claimed)
            continue;
        it[n++] = (struct item){ c->vbases[i].off, b->nvsize,
                                 cx_fmt("char __cx_vbase%d[%ld]", i,
                                        b->nvsize) };
    }
    for (int i = 0; i < c->nfields; i++) {
        struct cfield *fl = c->fields[i];
        long sz = ct_is_ref(fl->type) ? 8 : ct_size(fl->type);
        it[n++] = (struct item){ fl->off, sz, cdecl(fl->type,
                                                    field_cname(fl)) };
    }
    qsort(it, (size_t)n, sizeof *it, item_cmp);
    sb_printf(out, "struct %s {\n", c->cname);
    long at = 0;
    int pad = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].off < at)
            cx_error(NULL, "internal: overlapping layout in '%s'", c->name);
        if (it[i].off > at)
            sb_printf(out, "    char __cx_pad%d[%ld];\n", pad++,
                      it[i].off - at);
        sb_printf(out, "    %s;\n", it[i].decl);
        at = it[i].off + it[i].size;
    }
    if (c->size > at)
        sb_printf(out, "    char __cx_pad%d[%ld];\n", pad++, c->size - at);
    sb_printf(out, "} __attribute__((packed, aligned(%ld)));\n", c->align);
}

/* ---- vtables and RTTI (Itanium 2.5, 2.9.5) ---- */

static struct sb out_rtti_decl;

/* Where c's vtable and typeinfo live: 0 another unit (the one defining
 * its key function), 1 here and weak (no key function), 2 here. */
static int rtti_home(struct cclass *c)
{
    if (class_internal(c))
        return 1;
    if (!c->dynamic || !c->key)
        return 1;
    return c->key->defined ? 2 : 0;
}

static const char *rtti_storage(struct cclass *c)
{
    if (class_internal(c))
        return "static ";
    return rtti_home(c) == 1 ? "__attribute__((weak)) " : "";
}

static void need_rtti(struct cclass *c);

/* The typeinfo object of t: a class's (emitted by this unit or another),
 * or a fundamental type's — libsupc++ has those, and their pointers'. */
static const char *typeinfo_sym(struct cty *t)
{
    t = ct_unqual(t);
    if (t->k == CT_CLASS) {
        need_rtti(t->cls);
        return class_sym(t->cls, "_ZTI");
    }
    struct cty *b = t->k == CT_PTR ? ct_unqual(t->to) : t;
    if (t->k == CT_PTR && b->k == CT_CLASS)
        return ptr_typeinfo(t);
    if (b->k > CT_NULLPTR || (t->k == CT_PTR && ct_is_ref(t->to)))
        cx_error(NULL, "typeid of '%s' is not supported yet", ct_name(t));
    const char *sym = cx_fmt("_ZTI%s", mangle_type_alone(t));
    sb_printf(&out_rtti_decl, "extern void *%s[];\n", sym);
    return sym;
}

/* The typeinfo of a pointer to a class (a __pointer_type_info: its
 * pointee's cv as flags, and the class's typeinfo), weak in every unit
 * that needs it, as g++ makes it. */
static const char **ptr_ti_done;
static int nptr_ti_done;

static const char *ptr_typeinfo(struct cty *t)
{
    struct cty *pointee = t->to;
    const char *m = mangle_type_alone(t);
    const char *sym = cx_fmt("_ZTI%s", m);
    for (int i = 0; i < nptr_ti_done; i++)
        if (strcmp(ptr_ti_done[i], sym) == 0)
            return sym;
    ptr_ti_done = xrealloc(ptr_ti_done, (size_t)(nptr_ti_done + 1) *
                                        sizeof *ptr_ti_done);
    ptr_ti_done[nptr_ti_done++] = sym;
    const char *cls = typeinfo_sym(ct_unqual(pointee));
    int internal = class_internal(ct_unqual(pointee)->cls);
    const char *st = internal ? "static " : "__attribute__((weak)) ";
    long flags = ((pointee->q & CQ_CONST) ? 1 : 0) |
                 ((pointee->q & CQ_VOLATILE) ? 2 : 0);
    sb_printf(&out_rtti_decl, "%svoid *%s[4];\nextern void "
                              "*_ZTVN10__cxxabiv119__pointer_type_infoE[];\n",
              internal ? "static " : "extern ", sym);
    sb_printf(&out_rtti, "%schar _ZTS%s[] = \"%s\";\n", st, m, m);
    sb_printf(&out_rtti, "%svoid *%s[4] = { (void *)((char *)"
                         "_ZTVN10__cxxabiv119__pointer_type_infoE + 16), "
                         "(void *)_ZTS%s, (void *)%ldL, (void *)%s };\n",
              st, sym, m, flags, cls);
    return sym;
}

static void need_rtti(struct cclass *c)
{
    if (c->rtti_used)
        return;
    c->rtti_used = 1;
    for (int i = 0; i < c->nbases; i++)
        need_rtti(c->bases[i].cls);
}

/* c's typeinfo object: a __class_type_info (no bases), a
 * __si_class_type_info (one public non-virtual base at offset 0), else a
 * __vmi_class_type_info with each base's offset and flags. */
static void emit_rtti(struct cclass *c)
{
    c->rtti_done = 1;
    const char *ti = class_sym(c, "_ZTI"), *ts = class_sym(c, "_ZTS");
    int home = rtti_home(c);
    int si = c->nbases == 1 && !c->bases[0].is_virtual &&
             c->bases[0].access == CA_PUBLIC && c->bases[0].off == 0;
    const char *kind = c->nbases == 0 ? "17__class_type_info"
                       : si ? "20__si_class_type_info"
                       : "21__vmi_class_type_info";
    int words = c->nbases == 0 ? 2 : si ? 3 : 3 + 2 * c->nbases;
    if (home == 0) {
        sb_printf(&out_rtti_decl, "extern void *%s[];\n", ti);
        return;
    }
    const char *st = rtti_storage(c);
    if (!class_internal(c))
        sb_printf(&out_rtti_decl, "extern void *%s[%d];\n", ti, words);
    else
        sb_printf(&out_rtti_decl, "static void *%s[%d];\n", ti, words);
    sb_printf(&out_rtti_decl, "extern void *_ZTVN10__cxxabiv1%sE[];\n", kind);
    const char *mangled = class_internal(c) ? cx_fmt("*%s", c->cname)
                                            : mangle_class_name(c);
    sb_printf(&out_rtti, "%schar %s[] = \"%s\";\n", st, ts, mangled);
    sb_printf(&out_rtti, "%svoid *%s[%d] = { (void *)((char *)"
                         "_ZTVN10__cxxabiv1%sE + 16), (void *)%s", st, ti,
              words, kind, ts);
    if (si) {
        sb_printf(&out_rtti, ", (void *)%s", class_sym(c->bases[0].cls,
                                                       "_ZTI"));
    } else if (c->nbases) {
        long flags = class_rtti_flags(c);
        sb_printf(&out_rtti, ", (void *)%ldL",
                  flags | ((long)c->nbases << 32));
        for (int i = 0; i < c->nbases; i++) {
            struct cbase *b = &c->bases[i];
            /* a virtual base's offset is where the vtable holds it */
            long at = b->is_virtual ? class_vbindex(c, b->cls) : b->off;
            long of = (long)((unsigned long)at << 8) |
                      (b->access == CA_PUBLIC ? 2 : 0) |
                      (b->is_virtual ? 1 : 0);
            sb_printf(&out_rtti, ", (void *)%s, (void *)%ldL",
                      class_sym(b->cls, "_ZTI"), of);
        }
    }
    sb_put(&out_rtti, " };\n");
}

static struct sb out_thunks;

/* The thunks written so far in this unit (one definition each). */
static const char **thunks_done;
static int nthunks_done;

/* A this-adjusting thunk for f: `this += delta`, then when vcall `this +=`
 * the vcall offset its vtable holds at vcall — Itanium's _ZTh<delta>_ and
 * _ZTv<delta>_<vcall>_. */
static const char *thunk_for(struct cfunc *f, int deleting, long delta,
                             long vcall, struct cclass *c)
{
    const char *target = fn_name(f, f->is_dtor ? (deleting ? 0 : 1) : 1);
    const char *d = cx_fmt("%s%ld", delta < 0 ? "n" : "",
                           delta < 0 ? -delta : delta);
    const char *name;
    if (class_internal(c) || !strncmp(target, "__cx", 4))
        name = cx_fmt("%s_thunk%s_%ld", target, d, -vcall);
    else if (vcall)
        name = cx_fmt("_ZTv%s_n%ld_%s", d, -vcall, target + 2);
    else
        name = cx_fmt("_ZTh%s_%s", d, target + 2);
    for (int i = 0; i < nthunks_done; i++)
        if (strcmp(thunks_done[i], name) == 0)
            return name;
    thunks_done = xrealloc(thunks_done, (size_t)(nthunks_done + 1) *
                                        sizeof *thunks_done);
    thunks_done[nthunks_done++] = name;
    struct sb args = { 0, 0, 0 };
    int sret = class_indirect(f->type->to);
    if (sret)
        sb_put(&args, "__cx_sret, ");
    sb_printf(&args, "(%s)__cx_t", ctype(ct_ptr(ct_class(f->cls))));
    for (int i = 0; i < f->type->np; i++)
        sb_printf(&args, ", %s", cx_fmt("__cx_p%d", i));
    struct cfunc tmp = *f;
    tmp.params = NULL;                 /* the thunk's own parameter names */
    const char *st = class_internal(c) ? "static " : "__attribute__((weak)) ";
    sb_printf(&out_decls, "%s%s;\n", class_internal(c) ? "static " : "",
              func_header(&tmp, name, 0));
    sb_printf(&out_thunks, "%s%s\n{\nchar *__cx_t = (char *)this + %ldL;\n",
              st, func_header(&tmp, name, 1), delta);
    if (vcall)
        sb_printf(&out_thunks, "__cx_t += *(long *)(*(char **)__cx_t + "
                               "%ldL);\n", vcall);
    sb_printf(&out_thunks, "%s%s(%s);\n}\n",
              f->type->to->k == CT_VOID && !sret ? "" : "return ", target,
              sb_str(&args));
    return name;
}

static int any_pure;

/* A vtable group as an array: each vtable's vcall and vbase offsets,
 * offset to top, typeinfo, then its functions. */
static void emit_group(struct sb *b, const char *st, const char *sym,
                       struct vgroup *g, struct cclass *c)
{
    sb_printf(b, "%svoid *%s[%d] = {", st, sym, g->words);
    const char *sep = "";
    for (int i = 0; i < g->n; i++) {
        struct vtbl *v = &g->v[i];
        for (int k = 0; k < v->npre; k++) {
            sb_printf(b, "%s (void *)%ldL", sep, v->pre[k]);
            sep = ",";
        }
        sb_printf(b, "%s (void *)%ldL, (void *)%s", sep, v->ott,
                  typeinfo_sym(ct_class(v->rtti)));
        sep = ",";
        for (int k = 0; k < v->nfns; k++) {
            struct vfn *e = &v->fns[k];
            const char *fn;
            if (e->pure) {
                fn = "__cxa_pure_virtual";
                any_pure = 1;
            } else if (!e->f) {
                fn = "0";
            } else {
                need_fn(e->f);
                fn = e->delta || e->vcall
                     ? thunk_for(e->f, e->deleting, e->delta, e->vcall, c)
                     : fn_name(e->f, e->f->is_dtor ? (e->deleting ? 0 : 1)
                                                   : 1);
            }
            sb_printf(b, ", (void *)%s", fn);
        }
    }
    sb_put(b, " };\n");
}

/* The construction vtable group of base cg in c: _ZTC<c><off>_<base>. */
static const char *ctor_group_sym(struct cclass *c, struct ctorgrp *cg)
{
    if (class_internal(c))
        return cx_fmt("%s_%ld_%s", class_sym(c, "_ZTC"), cg->off,
                      cg->base->cname);
    return cx_fmt("_ZTC%s%ld_%s", mangle_class_name(c), cg->off,
                  mangle_class_name(cg->base));
}

static void emit_vtable(struct cclass *c)
{
    c->vtable_done = 1;
    need_rtti(c);
    const char *tv = class_sym(c, "_ZTV"), *tt = class_sym(c, "_ZTT");
    int home = rtti_home(c);
    if (home == 0) {
        sb_printf(&out_rtti_decl, "extern void *%s[];\n", tv);
        if (c->nvbases)
            sb_printf(&out_rtti_decl, "extern void *%s[];\n", tt);
        return;
    }
    struct sb b = { 0, 0, 0 };
    const char *st = rtti_storage(c);
    emit_group(&b, st, tv, vtable_group(c), c);
    if (c->nvbases) {
        /* the construction vtables its bases are built with, and the
         * VTT pointing into them (Itanium 2.6) */
        struct ctorgrp *cg;
        int ng = class_ctor_groups(c, &cg);
        for (int i = 0; i < ng; i++)
            emit_group(&b, st, ctor_group_sym(c, &cg[i]), cg[i].g, c);
        struct vttent *e;
        int n = class_vtt(c, &e);
        sb_printf(&b, "%svoid *%s[%d] = {", st, tt, n);
        for (int i = 0; i < n; i++)
            sb_printf(&b, "%s (void *)((void **)%s + %d)", i ? "," : "",
                      e[i].ctor < 0 ? tv : ctor_group_sym(c, &cg[e[i].ctor]),
                      e[i].point);
        sb_put(&b, " };\n");
    }
    sb_put(&out_vtables, sb_str(&b));
}

/* ---- the unit ---- */

char *cx_emit_unit(void)
{
    memset(&out_types, 0, sizeof out_types);
    memset(&out_decls, 0, sizeof out_decls);
    memset(&out_vars, 0, sizeof out_vars);
    memset(&out_code, 0, sizeof out_code);
    memset(&out_init, 0, sizeof out_init);
    memset(&out_rettypes, 0, sizeof out_rettypes);
    nwork = 0;
    need_atexit = need_guard = 0;
    need_pmf = 0;
    any_vtable = any_pure = need_dyncast = 0;
    memset(&out_rtti, 0, sizeof out_rtti);
    memset(&out_rtti_decl, 0, sizeof out_rtti_decl);
    memset(&out_vtables, 0, sizeof out_vtables);
    memset(&out_thunks, 0, sizeof out_thunks);
    nthunks_done = 0;
    nptr_ti_done = 0;
    eh_on = cx_exceptions;
    eh_used = 0;
    ntemps = 0;
    struct fx top, *save;
    fx_begin(&top, &save);

    for (struct cfunc *f = cx_funcs; f; f = f->all_next)
        if (f->defined && !f->is_inline && !f->is_implicit)
            need_fn(f);
    /* a vtable homed here is emitted whether or not this unit uses it */
    for (int i = 0; i < cx_nclasses; i++)
        if (cx_classes[i]->dynamic && rtti_home(cx_classes[i]) == 2)
            need_vtable(cx_classes[i]);
    int progress = 1;
    int wi = 0;
    while (progress) {
        progress = 0;
        while (wi < nwork) {
            emit_function(work[wi++]);
            progress = 1;
        }
        for (int i = 0; any_vtable && i < cx_nclasses; i++) {
            struct cclass *c = cx_classes[i];
            if (c->vtable_used && !c->vtable_done) {
                emit_vtable(c);
                progress = 1;
            }
        }
        for (int i = 0; i < cx_ngvars; i++) {
            if (gvar_needed(cx_gvars[i])) {
                emit_gvar(cx_gvars[i]);
                progress = 1;
            }
        }
    }
    for (int i = 0; i < cx_nclasses; i++)
        if (cx_classes[i]->rtti_used && !cx_classes[i]->rtti_done)
            emit_rtti(cx_classes[i]);
    fx_end(save);

    struct sb out = { 0, 0, 0 };
    /* classes: tags first, then definitions in completion order */
    for (int i = 0; i < cx_nclasses; i++)
        sb_printf(&out, "%s %s;\n", cx_classes[i]->is_union ? "union"
                                                            : "struct",
                  cx_classes[i]->cname);
    for (int i = 0; i < cx_nclasses; i++) {
        struct cclass *c = cx_classes[i];
        if (c->explicit_layout) {
            emit_explicit_struct(&out, c);
            continue;
        }
        sb_printf(&out, "%s %s {\n", c->is_union ? "union" : "struct",
                  c->cname);
        int any = 0;
        for (int k = 0; k < c->nfields; k++) {
            struct cfield *fl = c->fields[k];
            if (fl->bitwidth >= 0) {
                sb_printf(&out, "    %s : %d;\n",
                          cdecl(fl->type, fl->name ? field_cname(fl) : ""),
                          fl->bitwidth);
            } else {
                sb_printf(&out, "    %s;\n",
                          cdecl(fl->type, field_cname(fl)));
            }
            if (fl->name)
                any = 1;
        }
        if (!any)
            sb_put(&out, "    char __cx_empty;\n");
        sb_put(&out, "}");
        if (c->packed)
            sb_put(&out, " __attribute__((packed))");
        if (c->align_attr)
            sb_printf(&out, " __attribute__((aligned(%ld)))", c->align_attr);
        sb_put(&out, ";\n");
    }
    if (need_atexit || need_guard || out_init.len)
        sb_put(&out, "extern void *__dso_handle;\n"
                     "int __cxa_atexit(void (*)(void *), void *, void *);\n");
    if (need_guard)
        sb_put(&out, "int __cxa_guard_acquire(long long *);\n"
                     "void __cxa_guard_release(long long *);\n"
                     "void __cxa_guard_abort(long long *);\n");
    /* __builtin_memset/memcpy are the libc functions to the C side, which
     * wants them declared */
    const char *parts[] = { sb_str(&out_code), sb_str(&out_vars),
                            sb_str(&out_init), sb_str(&out_thunks) };
    int use_set = 0, use_cpy = 0;
    for (int i = 0; i < 4; i++) {
        use_set |= parts[i] && strstr(parts[i], "__builtin_memset(") != NULL;
        use_cpy |= parts[i] && strstr(parts[i], "__builtin_memcpy(") != NULL;
    }
    if (use_set)
        sb_put(&out, "void *memset(void *, int, unsigned long);\n");
    if (eh_used) {
        /* the C++ ABI's exception functions — called through casts, so a
         * declaration of the program's own (<cxxabi.h>) serves as well */
        static const char *const abi[][2] = {
            { "__cxa_allocate_exception", "void *%s(unsigned long);\n" },
            { "__cxa_throw", "void %s(void *, void *, void (*)(void *));\n" },
            { "__cxa_rethrow", "void %s(void);\n" },
            { "__cxa_begin_catch", "void *%s(void *);\n" },
            { "__cxa_end_catch", "void %s(void);\n" },
            { "_Unwind_Resume", "void %s(void *);\n" },
            { "__cxa_call_terminate", "void %s(void *);\n" },
        };
        for (unsigned i = 0; i < sizeof abi / sizeof abi[0]; i++) {
            int theirs = 0;
            for (struct cfunc *f = cx_funcs; f && !theirs; f = f->all_next)
                theirs = f->declared && f->c_linkage &&
                         strcmp(f->name, abi[i][0]) == 0;
            if (!theirs)
                sb_printf(&out, abi[i][1], abi[i][0]);
        }
    }
    if (use_cpy)
        sb_put(&out, "void *memcpy(void *, const void *, unsigned long);\n");
    for (struct cfunc *f = cx_funcs; f; f = f->all_next)
        if (f->declared)
            emit_prototype(f);
    if (any_pure)
        sb_put(&out, "void __cxa_pure_virtual(void);\n");
    if (need_dyncast)
        sb_put(&out, "void *__dynamic_cast(void *, void *, void *, long);\n"
                     "void __cxa_bad_cast(void);\n");
    sb_put(&out, sb_str(&out_rettypes));
    sb_put(&out, sb_str(&out_decls));
    sb_put(&out, sb_str(&out_rtti_decl));
    sb_put(&out, sb_str(&out_rtti));
    sb_put(&out, sb_str(&out_vtables));
    if (need_pmf) {
        /* whatever above names it is written first */
        struct sb pre = { 0, 0, 0 };
        sb_put(&pre, "struct __cx_pmf { void *ptr; long adj; };\n");
        sb_put(&pre, sb_str(&out));
        out = pre;
    }
    /* every referenced variable declared before any definition uses it */
    for (int i = 0; i < cx_ngvars; i++) {
        struct cvar *v = cx_gvars[i];
        if (v->refd && !v->is_static)
            sb_printf(&out, "extern %s;\n", cdecl(v->type, v->cname));
    }
    sb_put(&out, sb_str(&out_vars));
    sb_put(&out, sb_str(&out_code));
    sb_put(&out, sb_str(&out_thunks));
    if (out_init.len) {
        sb_printf(&out, "static void __cx_global_init(void)\n{\n%s}\n",
                  sb_str(&out_init));
        sb_put(&out, "__attribute__((section(\".init_array\"), used)) "
                     "static void (*__cx_global_init_p)(void) = "
                     "__cx_global_init;\n");
    }
    return out.p ? out.p : cx_strdup("");
}
