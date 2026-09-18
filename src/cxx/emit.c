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
    if (f->cls && (f->cls->local || f->cls->anon)) {
        if (!f->cname) {
            f->cname = cx_fmt("__cx_lf%d", cx_uid());
            f->cname2 = cx_fmt("%s_base", f->cname);
        }
        return variant == 2 && (f->is_ctor || f->is_dtor) ? f->cname2
                                                          : f->cname;
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
        struct sb p = { 0, 0, 0 };
        for (int i = 0; i < t->np; i++)
            sb_printf(&p, "%s%s", i ? ", " : "", cdecl(t->params[i], ""));
        if (t->variadic)
            sb_put(&p, t->np ? ", ..." : "...");
        if (!t->np && !t->variadic)
            sb_put(&p, "void");
        return cdecl(t->to, cx_fmt("%s(%s)", inner, sb_str(&p)));
    }
    case CT_CLASS:
        return cx_fmt("%s%s %s%s%s", vol, t->cls->is_union ? "union"
                                                          : "struct",
                      t->cls->cname, sp, inner);
    case CT_ENUM:
        return cdecl(ct_qual(t->en->underlying, t->q), inner);
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
static struct cfunc **work;
static int nwork, capwork;
static struct cfunc *cur_fn;
static int need_atexit, need_guard;

static void need_fn(struct cfunc *f)
{
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

static void need_var(struct cvar *v)
{
    if (!v->is_local || v->is_static)
        v->refd = 1;
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
        if (fx->cond) {
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

static char *call_text(struct cexpr *e)
{
    need_fn(e->fn);
    char *t = cx_fmt("%s(%s)", fn_name(e->fn, 1), args_text(e->a, e->na, 0));
    return t;
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
            einit(&s, cx_fmt("(*%s)", p), el, e->init);
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
    if (e->init) {
        struct sb s = { 0, 0, 0 };
        einit(&s, cx_fmt("%s[__cx_i%d]", p, u), el, e->init);
        sb_printf(&b, "for (unsigned long __cx_i%d = 0; __cx_i%d < %s; "
                      "__cx_i%d++) { %s} ", u, u, n, u, sb_str(&s));
    }
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
    if (e->t && ct_is_integer(e->t) && e->k != E_INT && expr_const(e, &v))
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
    case E_CALL: {
        char *t = call_text(e);
        if (e->vc != VC_PRVALUE)
            return cx_fmt("(*%s)", t);
        return t;
    }
    case E_ICALL: {
        char *t = cx_fmt("(%s)(%s)", ev(e->a[0]), args_text(e->a, e->na, 1));
        if (e->vc != VC_PRVALUE)
            return cx_fmt("(*%s)", t);
        return t;
    }
    case E_BUILTIN: {
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
        return cx_fmt("(%s %s %s)", ev(e->a[0]), binop_text(e->op),
                      ev(e->a[1]));
    }
    case E_ASSIGN:
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
    case E_VAR:
        need_var(e->var);
        if (ct_is_ref(e->var->type))
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
    case E_CALL: case E_ICALL:
        if (e->vc == VC_PRVALUE)
            return materialize(e->t, e);
        return ev(e);
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
    case E_VAR:
        need_var(e->var);
        if (ct_is_ref(e->var->type))
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
    case E_CALL: case E_ICALL:
        if (e->vc != VC_PRVALUE) {
            if (e->k == E_CALL)
                return call_text(e);
            return cx_fmt("(%s)(%s)", ev(e->a[0]),
                          args_text(e->a, e->na, 1));
        }
        break;
    case E_CAST:
        if (e->lvcast)
            return cx_fmt("((%s)%s)", ctype(ct_ptr(e->t)), eaddr(e->a[0]));
        break;
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
    case E_CONSTRUCT:
        if (init->t->k == CT_ARRAY) {
            long n = 1;
            struct cty *el = init->t;
            while (el->k == CT_ARRAY) {
                n *= el->n;
                el = el->to;
            }
            int u = cx_uid();
            struct sb s = { 0, 0, 0 };
            einit(&s, cx_fmt("__cx_a%d[__cx_i%d]", u, u), el, init->init);
            sb_printf(b, "{ %s = (%s)&(%s); for (unsigned long __cx_i%d = 0; "
                         "__cx_i%d < %ldUL; __cx_i%d++) { %s} } ",
                      cdecl(ct_ptr(el), cx_fmt("__cx_a%d", u)),
                      ctype(ct_ptr(el)), dest, u, u, n, u, sb_str(&s));
            return;
        }
        if (init->zero)
            sb_printf(b, "__builtin_memset(&(%s), 0, sizeof(%s)); ", dest,
                      dest);
        if (init->fn) {
            need_fn(init->fn);
            sb_printf(b, "%s(&(%s)%s%s); ", fn_name(init->fn, 1), dest,
                      init->na ? ", " : "", args_text(init->a, init->na, 0));
        } else if (init->na == 1) {
            sb_printf(b, "%s = %s; ", dest, ev(init->a[0]));
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
            return expr_const(p->a[1], &v) && p->a[0]->t->k == CT_PTR &&
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
    if (e->t && ct_is_integer(e->t) && expr_const(e, &v))
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
    case E_CAST:
        if (e->lvcast)
            return 0;
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
            return c_const(e->a[0]) && expr_const(e->a[1], &v);
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

struct clean {
    char *text;
    struct cstmt *blk;
};

static struct clean *cleans;
static int ncleans, capcleans;
static int brk_mark[256], cont_mark[256];
static int nbrk, ncont;
static int line_now;
static const char *file_now;

static void push_clean(char *text, struct cstmt *blk)
{
    if (ncleans == capcleans) {
        capcleans = capcleans ? capcleans * 2 : 16;
        cleans = xrealloc(cleans, (size_t)capcleans * sizeof *cleans);
    }
    cleans[ncleans].text = text;
    cleans[ncleans].blk = blk;
    ncleans++;
}

static void run_cleans(struct sb *b, int mark)
{
    for (int i = ncleans - 1; i >= mark; i--)
        sb_printf(b, "%s\n", cleans[i].text);
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
    if (dyn)
        stmt_init(b, v->cname, t, ini);
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
                push_clean(d, s->blk);
            sb_printf(b, "%s = &%s;\n", cdecl(t, v->cname), name);
            return;
        }
        sb_printf(b, "%s%s = %s;\n", cdecl(t, v->cname), al,
                  p ? full_value(p) : "0");
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
                push_clean(d, s->blk);
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
        run_cleans(b, mark);
        ncleans = mark;
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
        run_cleans(b, mark);
        ncleans = mark;
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
            run_cleans(b, mark);
            ncleans = mark;
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
        run_cleans(b, mark);
        ncleans = mark;
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
        run_cleans(b, mark);
        ncleans = mark;
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
            if (!is_ancestor(cleans[i].blk, l->blk))
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
    }
}

static void emit_block_items(struct sb *b, struct cstmt *first)
{
    for (struct cstmt *s = first; s; s = s->next)
        emit_stmt(b, s);
}

/* ---- functions ---- */

static char *func_header(struct cfunc *f, const char *name, int named)
{
    struct sb p = { 0, 0, 0 };
    int any = 0;
    if (f->cls && !f->is_static) {
        sb_printf(&p, "%s", cdecl(ct_ptr(ct_class(f->cls)),
                                  named ? "this" : ""));
        any = 1;
    }
    struct cty *ft = f->type;
    for (int i = 0; i < ft->np; i++) {
        const char *pn = "";
        if (named)
            pn = f->params && f->params[i] ? f->params[i]->cname
                                           : cx_fmt("__cx_p%d", i);
        sb_printf(&p, "%s%s", any ? ", " : "", cdecl(ft->params[i], pn));
        any = 1;
    }
    if (ft->variadic)
        sb_put(&p, any ? ", ..." : "...");
    else if (!any)
        sb_put(&p, "void");
    return cdecl(ft->to, cx_fmt("%s(%s)", name, sb_str(&p)));
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
    const char *st = fn_storage(f);
    const char *sec = "";
    if (f->is_ctor || f->is_dtor) {
        sb_printf(&out_decls, "%s%s%s;\n", st, sec,
                  func_header(f, fn_name(f, 1), 0));
        sb_printf(&out_decls, "%s%s%s;\n", st, sec,
                  func_header(f, fn_name(f, 2), 0));
        return;
    }
    sb_printf(&out_decls, "%s%s%s;\n", st, sec,
              func_header(f, fn_name(f, 1), 0));
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
    struct sb b = { 0, 0, 0 };
    int special = f->is_ctor || f->is_dtor;
    const char *name = fn_name(f, special ? 2 : 1);
    const char *st = fn_storage(f);
    const char *sec = "";
    if (f->section)
        cx_warn(NULL, "section(\"%s\") on function '%s' is ignored",
                f->section, f->name);
    line_marker(&b, f->line, f->file);
    sb_printf(&b, "%s%s%s\n{\n", st, sec, func_header(f, name, 1));
    struct cclass *c = f->cls;
    if (f->is_ctor) {
        if (f->delegate) {
            stmt_init(&b, "(*this)", ct_class(c), f->delegate);
        } else if (f->meminit) {
            for (int i = 0; i < c->nfields; i++) {
                struct cfield *fl = c->fields[i];
                if (!f->meminit[i] || !fl->name)
                    continue;
                stmt_init(&b, cx_fmt("this->%s", field_cname(fl)), fl->type,
                          f->meminit[i]);
            }
        }
    }
    if (f->body)
        emit_block_items(&b, f->body->body);
    run_cleans(&b, 0);
    ncleans = 0;
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
    }
    if (!f->cls && !f->c_linkage && f->owner == cx_global &&
        strcmp(f->name, "main") == 0)
        sb_put(&b, "return 0;\n");
    sb_put(&b, "}\n");
    if (special) {
        /* the complete-object variant: the same, no virtual bases */
        struct sb args = { 0, 0, 0 };
        sb_put(&args, "this");
        for (int i = 0; i < f->type->np; i++)
            sb_printf(&args, ", %s", f->params && f->params[i]
                      ? f->params[i]->cname : cx_fmt("__cx_p%d", i));
        sb_printf(&b, "%s%s%s\n{\n%s(%s);\n}\n", st, sec,
                  func_header(f, fn_name(f, 1), 1), name, sb_str(&args));
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

/* ---- the unit ---- */

char *cx_emit_unit(void)
{
    memset(&out_types, 0, sizeof out_types);
    memset(&out_decls, 0, sizeof out_decls);
    memset(&out_vars, 0, sizeof out_vars);
    memset(&out_code, 0, sizeof out_code);
    memset(&out_init, 0, sizeof out_init);
    nwork = 0;
    need_atexit = need_guard = 0;
    ntemps = 0;
    struct fx top, *save;
    fx_begin(&top, &save);

    for (struct cfunc *f = cx_funcs; f; f = f->all_next)
        if (f->defined && !f->is_inline && !f->is_implicit)
            need_fn(f);
    int progress = 1;
    int wi = 0;
    while (progress) {
        progress = 0;
        while (wi < nwork) {
            emit_function(work[wi++]);
            progress = 1;
        }
        for (int i = 0; i < cx_ngvars; i++) {
            if (gvar_needed(cx_gvars[i])) {
                emit_gvar(cx_gvars[i]);
                progress = 1;
            }
        }
    }
    fx_end(save);

    struct sb out = { 0, 0, 0 };
    /* classes: tags first, then definitions in completion order */
    for (int i = 0; i < cx_nclasses; i++)
        sb_printf(&out, "%s %s;\n", cx_classes[i]->is_union ? "union"
                                                            : "struct",
                  cx_classes[i]->cname);
    for (int i = 0; i < cx_nclasses; i++) {
        struct cclass *c = cx_classes[i];
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
                     "void __cxa_guard_release(long long *);\n");
    for (struct cfunc *f = cx_funcs; f; f = f->all_next)
        if (f->declared)
            emit_prototype(f);
    sb_put(&out, sb_str(&out_decls));
    /* every referenced variable declared before any definition uses it */
    for (int i = 0; i < cx_ngvars; i++) {
        struct cvar *v = cx_gvars[i];
        if (v->refd && !v->is_static)
            sb_printf(&out, "extern %s;\n", cdecl(v->type, v->cname));
    }
    sb_put(&out, sb_str(&out_vars));
    sb_put(&out, sb_str(&out_code));
    if (out_init.len) {
        sb_printf(&out, "static void __cx_global_init(void)\n{\n%s}\n",
                  sb_str(&out_init));
        sb_put(&out, "__attribute__((section(\".init_array\"), used)) "
                     "static void (*__cx_global_init_p)(void) = "
                     "__cx_global_init;\n");
    }
    return out.p ? out.p : cx_strdup("");
}
