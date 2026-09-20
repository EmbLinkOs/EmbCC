/* Name resolution and type checking. Sema's output contract: every
 * expression node carries a type, and every implicit conversion C
 * would perform is materialized as an explicit EXPR_CAST node — irgen
 * never guesses about widths or signedness, it just reads the tree.
 */
#include <setjmp.h>
#include <stdarg.h>
#include "sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../arch/aarch64/asm.h"
#include "../driver/util.h"
#include "../arch/target.h"
#include "ldfloat.h"
#include "type.h"
#include "uninit.h"
#include "w128.h"

struct vardef {
    const char *name;
    struct type *ty;
    int used;           /* an expression read or wrote it (-Wunused-variable) */
    int is_param;       /* a parameter, not a local (-Wunused-parameter) */
    int line, col;      /* where it was declared, for the warning */
    struct global *g;   /* non-NULL for a static local: it lives in
                         * static storage, not the frame */
    int active;         /* 0 once its block has closed */
    const char *asm_reg; /* a register-asm binding, else NULL */
    int user_align;     /* __attribute__((aligned(N))) on the local; 0 = none */
};

/* Block scoping without giving up unique frame slots: entries are never
 * removed — the index IS the variable's slot for the whole function —
 * they are only deactivated when their block ends. Lookup runs backward
 * so an inner declaration shadows an outer one. */
struct scope {
    struct vardef *vars;
    int n, cap;
    int block_start;    /* index where the innermost block began */
};

/* Where semantic analysis resumes after an error: the statement being
 * checked. NULL means the error stops the compile, as every semantic error
 * once did (docs/TOOLING.md T2). */
static jmp_buf *g_recover;
static int g_sema_errors;

/* The file whose code is being checked: a function defined in a header
 * belongs to that header, and its diagnostics must say so — the unit's own
 * name would send the reader to the #include line. */
static const char *g_file;

static const char *diag_file(struct unit *u)
{
    return g_file ? g_file : u->file;
}

/* Names already reported undeclared in this function: a name misspelt once
 * is usually used several times, and one diagnostic per use is noise. */
static const char **g_undeclared;
static int g_nundeclared, g_capundeclared;

static int reported_undeclared(const char *name)
{
    for (int i = 0; i < g_nundeclared; i++)
        if (strcmp(g_undeclared[i], name) == 0)
            return 1;
    if (g_nundeclared == g_capundeclared) {
        g_capundeclared = g_capundeclared ? g_capundeclared * 2 : 8;
        g_undeclared = xrealloc(g_undeclared,
                                (size_t)g_capundeclared * sizeof *g_undeclared);
    }
    g_undeclared[g_nundeclared++] = name;
    return 0;
}

int sema_error_count(void) { return g_sema_errors; }

/* A semantic error with an explain id (docs/TOOLING.md T6): the reader is
 * told what to type to learn the rule. */
static void sema_error_id(struct unit *u, int line, int col, const char *id,
                          const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_verror_at(diag_file(u), line, col, fmt, ap);
    va_end(ap);
    diag_set_id(id);
    g_sema_errors++;
    if (g_recover)
        longjmp(*g_recover, 1);
    exit(1);
}

/* A semantic error: recorded, then analysis resumes at the next statement,
 * so one run reports every independent problem. */
static void sema_error_at(struct unit *u, int line, int col,
                          const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_verror_at(diag_file(u), line, col, fmt, ap);
    va_end(ap);
    g_sema_errors++;
    if (g_recover)
        longjmp(*g_recover, 1);
    exit(1);
}

/* The same where the location is a declaration rather than a token, so
 * there is no column to point at. */
static void sema_error_line(struct unit *u, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_verror_at(diag_file(u), line, 0, fmt, ap);
    va_end(ap);
    g_sema_errors++;
    if (g_recover)
        longjmp(*g_recover, 1);
    exit(1);
}

static int scope_find(struct scope *sc, const char *name)
{
    for (int i = sc->n - 1; i >= 0; i--)
        if (sc->vars[i].active && strcmp(sc->vars[i].name, name) == 0)
            return i;
    return -1;
}

/* A redeclaration is an error only within the SAME block; an inner
 * block may shadow, exactly as C allows. */
static int scope_find_here(struct scope *sc, const char *name)
{
    for (int i = sc->n - 1; i >= sc->block_start; i--)
        if (sc->vars[i].active && strcmp(sc->vars[i].name, name) == 0)
            return i;
    return -1;
}

static int scope_add(struct scope *sc, const char *name, struct type *ty,
                     struct global *g)
{
    if (sc->n == sc->cap) {
        sc->cap = sc->cap ? sc->cap * 2 : 8;
        sc->vars = xrealloc(sc->vars, (size_t)sc->cap * sizeof *sc->vars);
    }
    sc->vars[sc->n].name = name;
    sc->vars[sc->n].ty = ty;
    sc->vars[sc->n].g = g;
    sc->vars[sc->n].active = 1;
    sc->vars[sc->n].used = 0;
    sc->vars[sc->n].is_param = 0;
    sc->vars[sc->n].line = 0;
    sc->vars[sc->n].col = 0;
    sc->vars[sc->n].asm_reg = NULL;
    sc->vars[sc->n].user_align = 0;
    return sc->n++;
}

/* -Wshadow: a declaration that hides one still in scope. Reported where
 * the new one is, with a note at the one it hides — the pair is the point.
 * A name that shadows nothing costs a scan of the active entries. */
static void warn_shadow(struct unit *u, struct scope *sc, const char *name,
                        int line, int col)
{
    if (!diag_warning_enabled("shadow") || !name || name[0] == '<')
        return;
    for (int i = sc->n - 1; i >= 0; i--)
        if (sc->vars[i].active && sc->vars[i].name &&
            strcmp(sc->vars[i].name, name) == 0) {
            diag_warn_opt(diag_file(u), line, col, "shadow",
                          "declaration of '%s' shadows %s", name,
                          sc->vars[i].is_param ? "a parameter"
                                               : "an earlier one");
            if (sc->vars[i].line)
                diag_note_at(diag_file(u), sc->vars[i].line, sc->vars[i].col,
                             "the one it hides is here");
            return;
        }
}

/* Returns the canonical node for a name: the first declaration, into
 * which any later declarations have been merged. */
static struct func *find_func(struct unit *u, const char *name)
{
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && strcmp(f->name, name) == 0)
            return f;
    return NULL;
}

/* __builtin_memcpy, __builtin_memmove, __builtin_memset with no
 * declaration of the libc function in sight: its prototype, as gcc knows
 * it, declared at the unit's start */
static void implicit_mem_decl(struct unit *u, const char *name)
{
    if (find_func(u, name))
        return;
    struct func *fd = xcalloc(1, sizeof *fd);
    fd->name = name;
    fd->file = u->file;
    fd->seq = -1;
    fd->declared = 1;
    fd->ret_ty = ty_ptr(ty_base(TY_VOID, 0));
    fd->nparams = 3;
    fd->param_tys[0] = ty_ptr(ty_base(TY_VOID, 0));
    fd->param_tys[1] = strcmp(name, "memset") == 0 ? ty_base(TY_INT, 0)
                                                   : ty_ptr(ty_base(TY_VOID, 0));
    fd->param_tys[2] = ty_base(TY_LONG, 1);            /* size_t */
    struct func **pp = &u->funcs;
    while (*pp)
        pp = &(*pp)->next;
    *pp = fd;
}

static struct global *find_global(struct unit *u, const char *name)
{
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && strcmp(g->name, name) == 0)
            return g;
    return NULL;
}

/* The source position of the function body being checked — a global is
 * visible inside it only if declared above it (C's rule). */
static int cur_body_seq;

/* ---- conversions ---- */

static int is_null_const(const struct expr *e)
{
    return e->kind == EXPR_NUM && e->num == 0;
}

/* IEEE-754 +infinity / quiet-NaN as doubles, built from their bit patterns so
 * the value is exact regardless of the host EmbCC runs on. Used to lower the
 * __builtin_inf/huge_val/nan family to a float constant. */
static double ieee_inf(void)
{
    union { unsigned long u; double d; } v;
    v.u = 0x7ff0000000000000UL;
    return v.d;
}
static double ieee_nan(void)
{
    union { unsigned long u; double d; } v;
    v.u = 0x7ff8000000000000UL;
    return v.d;
}

/* Wrap in an implicit cast node unless already exactly that type. */
static struct expr *mk_cast(struct expr *inner, struct type *to)
{
    if (ty_equal(inner->ty, to))
        return inner;
    struct expr *c = xcalloc(1, sizeof *c);
    c->kind = EXPR_CAST;
    c->line = inner->line;
    c->cast_ty = to;
    c->rhs = inner;
    c->ty = to;
    return c;
}

/* C's integer promotions: everything narrower than int becomes int
 * (all narrow values fit, so the promoted type is always signed). */
static struct type *promote(struct type *t)
{
    if (t->kind == TY_CHAR || t->kind == TY_SHORT)
        return ty_base(TY_INT, 0);
    return t;
}

/* The DEFAULT ARGUMENT promotions, which are the integer promotions
 * PLUS float -> double. Distinct from promote() on purpose: a variadic
 * callee reads a float argument as a double, so passing a bare float
 * would hand printf garbage. */
static struct type *default_arg_promote(struct type *t)
{
    if (t->kind == TY_FLOAT)
        return ty_base(TY_DOUBLE, 0);
    return promote(t);
}

/* Usual arithmetic conversions, LP64: ranks are int(32) and long(64);
 * long can represent every unsigned int, so mixed int/long keeps the
 * long's signedness. */
static struct type *arith_common(struct type *a, struct type *b)
{
    /* Floating types outrank every integer, and long double > double >
     * float — the usual arithmetic conversions, floating half first. */
    if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE)
        return ty_base(TY_LDOUBLE, 0);
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE)
        return ty_base(TY_DOUBLE, 0);
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT)
        return ty_base(TY_FLOAT, 0);
    a = promote(a);
    b = promote(b);
    int qa = a->kind == TY_INT128, qb = b->kind == TY_INT128;
    if (qa || qb)       /* __int128 outranks long, holds all its values */
        return ty_base(TY_INT128, qa && qb ? a->is_unsigned || b->is_unsigned
                                           : (qa ? a : b)->is_unsigned);
    int wa = ty_wide(a), wb = ty_wide(b);
    if (wa || wb) {
        int uns;
        if (wa && wb)
            uns = a->is_unsigned || b->is_unsigned;
        else
            uns = (wa ? a : b)->is_unsigned;
        return ty_base(TY_LONG, uns);
    }
    return ty_base(TY_INT, a->is_unsigned || b->is_unsigned);
}

static void need_scalar(struct unit *u, struct expr *e, const char *what)
{
    if (!ty_is_scalar(e->ty))
        sema_error_at(u, e->line, e->col, "%s needs a scalar value, got %s",
                   what, ty_name(e->ty));
}

static void need_integer(struct unit *u, struct expr *e, const char *what)
{
    if (!ty_is_integer(e->ty))
        sema_error_at(u, e->line, e->col, "%s needs an integer, got %s",
                   what, ty_name(e->ty));
}

static void need_arith(struct unit *u, struct expr *e, const char *what)
{
    if (!ty_is_arith(e->ty))
        sema_error_at(u, e->line, e->col,
                   "%s needs an arithmetic value, got %s", what,
                   ty_name(e->ty));
}

/* The conversions assignment performs (also used for arguments and
 * return values). Explicit casts are looser; this is the implicit set. */
/* SSE2 has no instruction converting between a 64-bit UNSIGNED integer
 * and a float: cvtsi2sd and cvttsd2si are both signed, so anything at
 * or above 2^63 would come out wrong. gcc emits a branchy fixup; EmbCC
 * refuses instead of quietly producing the wrong number (THE RULE).
 * Every other combination is exact: narrower integers are converted
 * through their 64-bit form. */
static struct expr *cx_cast(struct expr *x, struct type *to);

static struct expr *convert_assign(struct unit *u, struct expr *rhs,
                                   struct type *to, const char *ctx)
{
    if ((ty_is_complex(to) &&
         (ty_is_arith(rhs->ty) || ty_is_complex(rhs->ty))) ||
        (ty_is_complex(rhs->ty) && (ty_is_arith(to) || to->kind == TY_BOOL)))
        return cx_cast(rhs, to);
    if (to->kind == TY_STRUCT || rhs->ty->kind == TY_STRUCT) {
        if (!ty_equal(to, rhs->ty))
            sema_error_id(u, rhs->line, rhs->col, "E0003",
                          "%s: cannot convert %s to %s",
                       ctx, ty_name(rhs->ty), ty_name(to));
        return rhs; /* same struct type: passed/returned as its bytes */
    }
    /* Any scalar converts to _Bool implicitly (C99 6.3.1.2): the result is
     * 0 or 1. This includes pointers, which otherwise need an explicit cast
     * to an integer type. */
    if (to->kind == TY_BOOL && ty_is_scalar(rhs->ty))
        return mk_cast(rhs, to);
    if (ty_is_arith(to) && ty_is_arith(rhs->ty))
        return mk_cast(rhs, to);
    if (to->kind == TY_PTR) {
        if (rhs->ty->kind == TY_PTR &&
            (ty_equal(rhs->ty, to) || to->pointee->kind == TY_VOID ||
             rhs->ty->pointee->kind == TY_VOID))
            return mk_cast(rhs, to);
        /* Same-size integer pointees differing only in signedness
         * (`char *` vs `unsigned char *`): gcc warns but allows it, and real
         * code — string/buffer routines especially — relies on it. */
        if (rhs->ty->kind == TY_PTR &&
            ty_is_integer(to->pointee) && ty_is_integer(rhs->ty->pointee) &&
            ty_size(to->pointee) == ty_size(rhs->ty->pointee))
            return mk_cast(rhs, to);
        if (is_null_const(rhs))
            return mk_cast(rhs, to);
        sema_error_at(u, rhs->line, rhs->col,
                   "%s: cannot convert %s to %s without a cast",
                   ctx, ty_name(rhs->ty), ty_name(to));
    }
    if (ty_is_float(to) && rhs->ty->kind == TY_PTR)
        sema_error_at(u, rhs->line, rhs->col,
                   "%s: a pointer cannot become %s", ctx, ty_name(to));
    if (ty_is_integer(to) && rhs->ty->kind == TY_PTR)
        sema_error_at(u, rhs->line, rhs->col,
                   "%s: converting %s to %s needs an explicit cast",
                   ctx, ty_name(rhs->ty), ty_name(to));
    sema_error_at(u, rhs->line, rhs->col, "%s: cannot convert %s to %s",
               ctx, ty_name(rhs->ty), ty_name(to));
    return NULL;
}

static int is_lvalue(const struct expr *e)
{
    return e->kind == EXPR_VAR || e->kind == EXPR_DEREF ||
           e->kind == EXPR_MEMBER || e->kind == EXPR_COMPLIT;
}

static int const_fold(const struct expr *e, long *out);
struct scope;
static void check_atomic_call(struct unit *u, struct func *f,
                              struct scope *sc, struct expr *e,
                              enum atomic_kind ak);

/* A growing (offset, type, value) list of flattened initializer leaves —
 * defined here so both check_expr (compound literals) and check_stmt
 * (declarations) can build one. */
struct initbuf {
    struct initelem *v;
    int n, cap;
};
static void flatten_init(struct unit *u, struct func *f, struct scope *sc,
                         struct expr *init, struct type *ty, int off,
                         struct initbuf *out);
static void lower_static_bytes(struct unit *u, int line, int size,
                               struct initelem *v, int n,
                               const char **out_bytes,
                               struct greloc **out_rel, int *out_nrel);

/* A bit-field's value merged into its storage unit's nb bytes at p
 * (they start zeroed, so OR is enough and neighbours are kept): its low
 * `width` bits, at bit_off of p[0] — 17 bytes for a packed __int128's at
 * bit 1..7, the last one past what 128 bits hold. */
static void merge_bits(char *p, int nb, struct w128 val, int bit_off,
                       int width)
{
    struct w128 mask = w_shr(w_make(~0UL, ~0UL), 128 - width, 0);
    val.lo &= mask.lo;
    val.hi &= mask.hi;
    struct w128 lo = w_shl(val, bit_off);
    for (int b = 0; b < nb && b < 16; b++)
        p[b] |= (char)((b < 8 ? lo.lo : lo.hi) >> (8 * (b & 7)));
    if (nb == 17 && bit_off)
        p[16] |= (char)w_shr(val, 128 - bit_off, 0).lo;
}
static void check_stmt(struct unit *u, struct func *f, struct scope *sc,
                       struct stmt *s, int in_loop, int in_switch,
                       int at_sw_level);

/* Nonzero while lowering a static initializer (a file-scope global or a
 * static local). A compound literal met here has static storage: it becomes
 * an anonymous global rather than a stack slot. */
static int g_in_static_init;

/* Resolve `name` as a member of `base`, descending into any anonymous
 * struct/union members (C11 6.7.2.1p13: their members are reached as if
 * they belonged to the enclosing type). On success fills *out with the leaf
 * member — a copy, its offset made cumulative from `base` — and returns 1. */
static int find_member_deep(struct type *base, const char *name,
                            struct member *out, int base_off)
{
    for (int i = 0; i < base->nmembers; i++) {
        struct member *m = &base->members[i];
        if (m->name && strcmp(m->name, name) == 0) {
            *out = *m;
            out->off += base_off;
            return 1;
        }
        if (!m->name && !m->is_bitfield && m->ty->kind == TY_STRUCT &&
            find_member_deep(m->ty, name, out, base_off + m->off))
            return 1;
    }
    return 0;
}

/* Levenshtein distance, bounded — for "did you mean?" suggestions. Long names
 * are not worth diffing (capped at 999 = "no match"). */
static int edit_distance(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 63 || lb > 63)
        return 999;
    int prev[65], cur[65];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int del = prev[j] + 1, ins = cur[j - 1] + 1, sub = prev[j - 1] + cost;
            int m = del < ins ? del : ins;
            cur[j] = m < sub ? m : sub;
        }
        for (int j = 0; j <= lb; j++) prev[j] = cur[j];
    }
    return prev[lb];
}

/* The in-scope local, function, or global whose name is closest to `name`, if
 * one is close enough to be worth suggesting (a few edits, scaled to length),
 * else NULL. Powers the "did you mean 'x'?" note on an undeclared name. */
static const char *suggest_name(struct unit *u, struct scope *sc,
                                const char *name)
{
    const char *best = NULL;
    int bestd = 1000, nlen = (int)strlen(name);
    for (int i = 0; i < sc->n; i++)
        if (sc->vars[i].active) {
            int d = edit_distance(name, sc->vars[i].name);
            if (d > 0 && d < bestd) { bestd = d; best = sc->vars[i].name; }
        }
    for (struct func *fd = u->funcs; fd; fd = fd->next) {
        int d = edit_distance(name, fd->name);
        if (d > 0 && d < bestd) { bestd = d; best = fd->name; }
    }
    for (struct global *g = u->globals; g; g = g->next) {
        int d = edit_distance(name, g->name);
        if (d > 0 && d < bestd) { bestd = d; best = g->name; }
    }
    int thresh = nlen / 3 < 2 ? 2 : nlen / 3;
    return (best && bestd <= thresh) ? best : NULL;
}

/* ---- expression checking ---- */

/* ---- _Complex: lowered to its parts ----
 *
 * A `T _Complex` is laid out, passed and returned as struct { T re, im; }
 * (type.c ty_complex) — its ABI on both targets — so storage, copies, calls
 * and returns need nothing new. Its ARITHMETIC is lowered here, into
 * ordinary float operations on the two parts: a complex operand used more
 * than once is evaluated once into a hidden temp, the result is built in a
 * fresh hidden slot and is that slot's value. Multiplication and division
 * of two complex values call libgcc (__mulsc3/__muldc3/..., __divsc3/...),
 * as gcc does, for C99 Annex G's infinities and NaNs; a real operand is
 * combined part by part (Annex G again, and what gcc does).
 *
 * Inside a static initializer nothing is lowered — there is no frame to put
 * a temp in — only typed; lower_static_bytes folds it (cx_fold). */

static int const_fold_f(const struct expr *e, double *out);
static struct ldf *const_fold_ld(const struct expr *e);
static struct scope *g_cx_sc;   /* the function being checked (check_func) */

static int cx_lowering(void)
{
    return g_cx_sc && !g_in_static_init;
}

static struct expr *cx_node(enum expr_kind k, int line, struct type *ty)
{
    struct expr *e = xcalloc(1, sizeof *e);
    e->kind = k;
    e->line = line;
    e->ty = ty;
    e->desig_index = e->desig_index_hi = -1;
    return e;
}

static struct expr *cx_var(int idx, int line)
{
    struct expr *v = cx_node(EXPR_VAR, line, g_cx_sc->vars[idx].ty);
    v->name = g_cx_sc->vars[idx].name;
    v->var_index = idx;
    return v;
}

static struct expr *cx_assign(struct expr *lhs, struct expr *rhs)
{
    struct expr *a = cx_node(EXPR_ASSIGN, lhs->line, lhs->ty);
    a->lhs = lhs;
    a->rhs = rhs;
    return a;
}

static struct expr *cx_comma(struct expr *first, struct expr *then)
{
    if (!first) return then;
    struct expr *c = cx_node(EXPR_COMMA, then->line, then->ty);
    c->lhs = first;
    c->rhs = then;
    return c;
}

static struct expr *cx_bin(enum binop op, struct expr *a, struct expr *b,
                           struct type *ty)
{
    int cmp = op == B_EQ || op == B_NE || op == B_LAND || op == B_LOR;
    struct expr *e = cx_node(EXPR_BINOP, a->line,
                             cmp ? ty_base(TY_INT, 0) : ty);
    e->op = op;
    e->lhs = op == B_LAND || op == B_LOR ? a : mk_cast(a, ty);
    e->rhs = op == B_LAND || op == B_LOR ? b : mk_cast(b, ty);
    return e;
}

static struct expr *cx_neg(struct expr *a)
{
    struct expr *e = cx_node(EXPR_NEG, a->line, a->ty);
    e->rhs = a;
    return e;
}

static struct expr *cx_zero(struct type *t, int line)
{
    struct expr *z = cx_node(EXPR_FNUM, line, t);
    z->fnum = 0.0;
    return z;
}

/* part 0 (real) or 1 (imaginary) of a complex LVALUE-like expression */
static struct expr *cx_part(struct expr *base, int imag)
{
    struct type *ct = base->ty;
    struct expr *m = cx_node(EXPR_MEMBER, base->line, ct->celem);
    m->lhs = base;
    m->name = ct->members[imag].name;
    m->memb = &ct->members[imag];
    return m;
}

/* x, made safe to read more than once: a plain variable as it is,
 * anything else evaluated once into a temp (the assignment in *pre) */
static struct expr *cx_stable(struct expr *x, struct expr **pre)
{
    *pre = NULL;
    if ((x->kind == EXPR_VAR && !x->fref) || x->kind == EXPR_FNUM ||
        x->kind == EXPR_NUM)
        return x;
    int t = scope_add(g_cx_sc, "<complex>", x->ty, NULL);
    *pre = cx_assign(cx_var(t, x->line), x);
    return cx_var(t, x->line);
}

/* A complex value of type ct built from its parts, in a fresh slot. */
static struct expr *cx_make(struct type *ct, struct expr *re,
                            struct expr *im, int line)
{
    int t = scope_add(g_cx_sc, "<complex>", ct, NULL);
    struct expr *a1 = cx_assign(cx_part(cx_var(t, line), 0),
                                mk_cast(re, ct->celem));
    struct expr *a2 = cx_assign(cx_part(cx_var(t, line), 1),
                                mk_cast(im, ct->celem));
    return cx_comma(a1, cx_comma(a2, cx_var(t, line)));
}

/* An operand's parts, converted to T: *re always, *im NULL for a real one.
 * The side effects that must run first are returned. */
static struct expr *cx_parts(struct expr *x, struct type *T,
                             struct expr **re, struct expr **im)
{
    struct expr *pre;
    if (ty_is_complex(x->ty)) {
        struct expr *s = cx_stable(x, &pre);
        *re = mk_cast(cx_part(s, 0), T);
        *im = mk_cast(cx_part(s, 1), T);
    } else {
        struct expr *s = cx_stable(mk_cast(x, T), &pre);
        *re = s;
        *im = NULL;
    }
    return pre;
}

/* The libgcc routine for a complex * or / in element type T. */
static struct func *cx_helper(int div, struct type *T)
{
    static struct func *made[8];
    const char *name;
    int k = (T->kind == TY_FLOAT ? 0 : T->kind == TY_DOUBLE ? 1 : 2) * 2 + div;
    if (T->kind == TY_LDOUBLE && target_get() == TARGET_AARCH64)
        k = 6 + div;
    static const char *const names[8] = {
        "__mulsc3", "__divsc3", "__muldc3", "__divdc3",
        "__mulxc3", "__divxc3", "__multc3", "__divtc3" };
    name = names[k];
    if (made[k]) return made[k];
    struct func *h = xcalloc(1, sizeof *h);
    h->name = name;
    h->ret_ty = ty_complex(T);
    h->nparams = 4;
    for (int i = 0; i < 4; i++)
        h->param_tys[i] = T;
    h->declared = 1;
    h->used = 1;
    made[k] = h;
    return h;
}

static struct expr *cx_call(int div, struct type *T, struct expr *a,
                            struct expr *b, struct expr *c, struct expr *d)
{
    struct func *h = cx_helper(div, T);
    struct expr *call = cx_node(EXPR_CALL, a->line, h->ret_ty);
    call->callee = h;
    call->args[0] = mk_cast(a, T);
    call->args[1] = mk_cast(b, T);
    call->args[2] = mk_cast(c, T);
    call->args[3] = mk_cast(d, T);
    call->nargs = 4;
    return call;
}

/* The element type two operands (real or complex) combine in. */
static struct type *cx_common(struct type *a, struct type *b)
{
    return arith_common(ty_is_complex(a) ? a->celem : a,
                        ty_is_complex(b) ? b->celem : b);
}

/* x != 0 for a complex x: either part nonzero (an int). */
static struct expr *cx_truth(struct expr *x)
{
    struct expr *re, *im;
    struct expr *pre = cx_parts(x, x->ty->celem, &re, &im);
    struct type *T = x->ty->celem;
    return cx_comma(pre, cx_bin(B_LOR, cx_bin(B_NE, re, cx_zero(T, x->line), T),
                                cx_bin(B_NE, im, cx_zero(T, x->line), T), T));
}

/* x converted to `to`, when either side is complex (C99 6.3.1.7): a real
 * becomes (x, 0); a complex loses its imaginary part going to a real type,
 * and to _Bool is "either part nonzero". */
static struct expr *cx_cast(struct expr *x, struct type *to)
{
    if (ty_equal(x->ty, to)) return x;
    if (!cx_lowering()) {                    /* static: fold later */
        struct expr *c = cx_node(EXPR_CAST, x->line, to);
        c->cast_ty = to;
        c->rhs = x;
        return c;
    }
    if (ty_is_complex(to)) {
        struct expr *re, *im;
        struct expr *pre = cx_parts(x, to->celem, &re, &im);
        return cx_comma(pre, cx_make(to, re, im ? im : cx_zero(to->celem, x->line),
                                     x->line));
    }
    if (to->kind == TY_BOOL)
        return cx_truth(x);
    if (to->kind == TY_VOID) {
        struct expr *c = cx_node(EXPR_CAST, x->line, to);
        c->cast_ty = to;
        c->rhs = x;
        return c;
    }
    struct expr *re, *im;
    struct expr *pre = cx_parts(x, x->ty->celem, &re, &im);
    return cx_comma(pre, mk_cast(re, to));
}

/* e (a BINOP with at least one complex operand, both checked) lowered in
 * place. */
static void cx_binop(struct unit *u, struct expr *e)
{
    struct type *lt = e->lhs->ty, *rt = e->rhs->ty;
    if (!ty_is_arith(lt) && !ty_is_complex(lt))
        sema_error_at(u, e->line, e->col, "invalid operand %s to a complex "
                "operation", ty_name(lt));
    if (!ty_is_arith(rt) && !ty_is_complex(rt))
        sema_error_at(u, e->line, e->col, "invalid operand %s to a complex "
                "operation", ty_name(rt));
    if (e->op == B_LAND || e->op == B_LOR) {
        if (!cx_lowering()) { e->ty = ty_base(TY_INT, 0); return; }
        if (ty_is_complex(lt)) e->lhs = cx_truth(e->lhs);
        if (ty_is_complex(rt)) e->rhs = cx_truth(e->rhs);
        e->ty = ty_base(TY_INT, 0);
        return;
    }
    if (e->op != B_ADD && e->op != B_SUB && e->op != B_MUL &&
        e->op != B_DIV && e->op != B_EQ && e->op != B_NE)
        sema_error_at(u, e->line, e->col,
                "a complex value only takes + - * / == and !=");
    struct type *T = cx_common(lt, rt), *CT = ty_complex(T);
    int eq = e->op == B_EQ || e->op == B_NE;
    if (!cx_lowering()) { e->ty = eq ? ty_base(TY_INT, 0) : CT; return; }

    struct expr *ar, *ai, *br, *bi;
    struct expr *pa = cx_parts(e->lhs, T, &ar, &ai);
    struct expr *pb = cx_parts(e->rhs, T, &br, &bi);
    int line = e->line;
    struct expr *res;
    switch (e->op) {
    case B_ADD: case B_SUB: {
        struct expr *im;
        if (ai && bi) im = cx_bin(e->op, ai, bi, T);
        else if (ai) im = ai;
        else im = e->op == B_ADD ? bi : cx_neg(bi);
        res = cx_make(CT, cx_bin(e->op, ar, br, T), im, line);
        break;
    }
    case B_MUL:
        if (ai && bi)
            res = cx_call(0, T, ar, ai, br, bi);
        else if (ai)
            res = cx_make(CT, cx_bin(B_MUL, ar, br, T), cx_bin(B_MUL, ai, br, T),
                          line);
        else
            res = cx_make(CT, cx_bin(B_MUL, ar, br, T), cx_bin(B_MUL, ar, bi, T),
                          line);
        break;
    case B_DIV:
        if (bi)
            res = cx_call(1, T, ar, ai ? ai : cx_zero(T, line), br, bi);
        else
            res = cx_make(CT, cx_bin(B_DIV, ar, br, T), cx_bin(B_DIV, ai, br, T),
                          line);
        break;
    default: {  /* == / != : both parts equal (a real's imaginary part is 0) */
        struct expr *i1 = ai ? ai : cx_zero(T, line);
        struct expr *i2 = bi ? bi : cx_zero(T, line);
        res = e->op == B_EQ
              ? cx_bin(B_LAND, cx_bin(B_EQ, ar, br, T), cx_bin(B_EQ, i1, i2, T), T)
              : cx_bin(B_LOR, cx_bin(B_NE, ar, br, T), cx_bin(B_NE, i1, i2, T), T);
        break;
    }
    }
    *e = *cx_comma(pa, cx_comma(pb, res));
}

/* `lhs op= rhs` or `++`/`--` where the target or the operand is complex:
 * lhs = lhs op rhs, the lvalue's address taken once. Returns the lowered
 * expression. `post` gives x++'s value (the old one). */
static struct expr *cx_update(struct unit *u, struct expr *lhs, enum binop op,
                              struct expr *rhs, int post)
{
    struct expr *pre = NULL, *target = lhs;
    if (!(lhs->kind == EXPR_VAR && !lhs->fref)) {
        /* p = &lhs; then *p twice */
        struct type *pt = ty_ptr(lhs->ty);
        struct expr *addr = cx_node(EXPR_ADDR, lhs->line, pt);
        addr->rhs = lhs;
        int t = scope_add(g_cx_sc, "<complex>", pt, NULL);
        pre = cx_assign(cx_var(t, lhs->line), addr);
        target = cx_node(EXPR_DEREF, lhs->line, lhs->ty);
        target->rhs = cx_var(t, lhs->line);
    }
    struct expr *old = NULL;
    if (post) {
        int t = scope_add(g_cx_sc, "<complex>", lhs->ty, NULL);
        pre = cx_comma(pre, cx_assign(cx_var(t, lhs->line), target));
        old = cx_var(t, lhs->line);
    }
    struct expr *val = cx_node(EXPR_BINOP, lhs->line, NULL);
    val->op = op;
    val->lhs = target;
    val->rhs = rhs;
    cx_binop(u, val);
    struct expr *as = cx_assign(target, cx_cast(val, lhs->ty));
    return cx_comma(pre, old ? cx_comma(as, old) : as);
}

/* Fold a static complex (or real) constant: its parts, in fmt. Only what a
 * static initializer reasonably holds — literals, casts, + and -, negation,
 * and scaling by a real; a complex * or / is left to run time (refused). */
static int cx_fold_real(const struct expr *e, enum ldf_fmt fmt, struct ldf **out);
static int cx_fold(const struct expr *e, enum ldf_fmt fmt, struct ldf **re,
                   struct ldf **im)
{
    if (!ty_is_complex(e->ty)) {
        *im = ldf_from_int(0, 0);
        return cx_fold_real(e, fmt, re);
    }
    struct ldf *ar, *ai, *br, *bi;
    switch (e->kind) {
    case EXPR_FNUM:                          /* an imaginary constant */
        *re = ldf_from_int(0, 0);
        *im = ldf_round(e->ldv ? e->ldv : ldf_from_double(
                  e->ty->celem->kind == TY_FLOAT ? (double)(float)e->fnum
                                                 : e->fnum), fmt);
        return 1;
    case EXPR_CAST:
        if (!cx_fold(e->rhs, fmt, &ar, &ai)) return 0;
        *re = ldf_round(ar, fmt);
        *im = ldf_round(ai, fmt);
        return 1;
    case EXPR_NEG:
        if (!cx_fold(e->rhs, fmt, &ar, &ai)) return 0;
        *re = ldf_neg(ar);
        *im = ldf_neg(ai);
        return 1;
    case EXPR_BINOP: {
        int lc = ty_is_complex(e->lhs->ty), rc = ty_is_complex(e->rhs->ty);
        if (!cx_fold(e->lhs, fmt, &ar, &ai) || !cx_fold(e->rhs, fmt, &br, &bi))
            return 0;
        if (e->op == B_ADD || e->op == B_SUB) {
            int o = e->op == B_ADD ? '+' : '-';
            *re = ldf_binop(o, ar, br, fmt);
            *im = lc && rc ? ldf_binop(o, ai, bi, fmt)
                 : lc ? ai : (o == '+' ? bi : ldf_neg(bi));
            return 1;
        }
        if (e->op == B_MUL && !(lc && rc)) {
            *re = ldf_binop('*', ar, br, fmt);
            *im = ldf_binop('*', lc ? ai : ar, lc ? br : bi, fmt);
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int cx_fold_real(const struct expr *e, enum ldf_fmt fmt, struct ldf **out)
{
    double d;
    long iv;
    if (e->ty->kind == TY_LDOUBLE) {
        struct ldf *x = const_fold_ld(e);
        if (!x) return 0;
        *out = ldf_round(x, fmt);
        return 1;
    }
    if (ty_is_float(e->ty)) {
        if (!const_fold_f(e, &d)) return 0;
        if (e->ty->kind == TY_FLOAT) d = (double)(float)d;
        *out = ldf_round(ldf_from_double(d), fmt);
        return 1;
    }
    if (!const_fold(e, &iv)) return 0;
    *out = ldf_round(ldf_from_int(iv, e->ty->is_unsigned), fmt);
    return 1;
}

/* Get a variably modified type ready for irgen: type-check every VLA
 * length in it (innermost first — `int a[n][m]`'s row size needs m before
 * the whole needs n) and give each VLA node the hidden slot its byte size
 * lives in. A node shared by two declarations (a block-scope typedef) is
 * prepared once. */
static void check_expr(struct unit *u, struct func *f, struct scope *sc,
                       struct expr *e);

static void vla_prepare(struct unit *u, struct func *f, struct scope *sc,
                        struct type *t)
{
    if (!t || (t->kind != TY_PTR && t->kind != TY_ARRAY))
        return;
    vla_prepare(u, f, sc, t->pointee);
    if (!ty_is_vla(t) || t->vla_size >= 0)
        return;
    check_expr(u, f, sc, t->vla_len);
    if (!ty_is_integer(t->vla_len->ty))
        sema_error_at(u, t->vla_len->line, t->vla_len->col,
                "size of array has non-integer type %s",
                ty_name(t->vla_len->ty));
    t->vla_len = mk_cast(t->vla_len, ty_base(TY_LONG, 0));
    t->vla_size = scope_add(sc, "<vla size>", ty_base(TY_LONG, 1), NULL);
}

static void check_expr(struct unit *u, struct func *f, struct scope *sc,
                       struct expr *e)
{
    switch (e->kind) {
    case EXPR_NUM:
    case EXPR_FNUM:
        /* type assigned by the parser from the literal's shape; an
         * imaginary constant is the complex 0 + v*i */
        if (e->kind == EXPR_FNUM && e->imag && cx_lowering()) {
            struct expr *v = cx_node(EXPR_FNUM, e->line, e->ty->celem);
            v->fnum = e->fnum;
            v->ldv = e->ldv;
            *e = *cx_make(e->ty, cx_zero(e->ty->celem, e->line), v, e->line);
        }
        break;
    case EXPR_REAL:
    case EXPR_IMAG: {
        /* __real__ x / __imag__ x: a complex's part (an lvalue when x is);
         * of a real x, x and 0 */
        int im = e->kind == EXPR_IMAG;
        check_expr(u, f, sc, e->rhs);
        struct expr *x = e->rhs;
        if (!ty_is_complex(x->ty)) {
            need_arith(u, x, im ? "__imag__" : "__real__");
            if (!im) { *e = *x; break; }
            struct expr *z = cx_node(EXPR_NUM, e->line, x->ty);
            if (ty_is_float(x->ty)) z->kind = EXPR_FNUM;
            *e = *cx_comma(x, z);       /* x still runs, for its effects */
            break;
        }
        if (!cx_lowering()) {
            e->ty = x->ty->celem;
            break;
        }
        if (is_lvalue(x)) {
            *e = *cx_part(x, im);
        } else {
            struct expr *pre;
            struct expr *s2 = cx_stable(x, &pre);
            *e = *cx_comma(pre, cx_part(s2, im));
        }
        break;
    }
    case EXPR_STR: {
        /* char[N] (or wchar_t/char16_t/char32_t[N] for a wide literal),
         * decaying to a pointer like any array (sizeof sees the array through
         * `undecayed`). e->num is the element count including the NUL. */
        struct type *elem = e->str_width == 4
                              ? (e->str_prefix == 'U' ? ty_base(TY_INT, 1)  /* char32_t */
                                                      : ty_wchar())
                          : e->str_width == 2 ? ty_base(TY_SHORT, 1)        /* char16_t */
                          : ty_plain_char();
        e->undecayed = ty_array(elem, (int)e->num);
        e->ty = ty_ptr(elem);
        break;
    }
    case EXPR_VAR: {
        int i = scope_find(sc, e->name);
        if (i >= 0) {
            sc->vars[i].used = 1;     /* -Wunused-variable: it was read */
            e->var_index = i;
            e->ty = sc->vars[i].ty;
            e->gref = sc->vars[i].g; /* set for a static local */
            e->asm_reg = sc->vars[i].asm_reg; /* register-asm binding */
        } else {
            /* enumerators fold to their constant right here */
            struct econst *ec = u->econsts;
            for (; ec; ec = ec->next)
                if (strcmp(ec->name, e->name) == 0)
                    break;
            /* '<=' not '<': an enum is COMPLETE before the declarator that
             * uses it in the same declaration (`enum { A, B } g = B;`), so a
             * same-item (same seq) reference is legal -- the same reasoning the
             * global self-reference below uses. A truly earlier use (a lower
             * seq than the enum's) is still rejected. */
            if (ec && ec->seq <= cur_body_seq) {
                e->kind = EXPR_NUM;
                e->num = ec->val;
                e->ty = ty_base(TY_INT, 0);
                break;
            }
            if (ec)
                sema_error_at(u, e->line, e->col,
                           "enumerator '%s' is used before its "
                           "declaration", e->name);
            struct global *g = find_global(u, e->name);
            /* '<=' not '<': a global's own name is in scope within its
             * initializer (C11 6.2.1p7), so `void *p = &p` is legal; seqs
             * are unique, so this only ever admits that self-reference. */
            if (g && g->seq <= cur_body_seq) {
                e->gref = g;
                g->used = 1;
                e->ty = g->ty;
            } else if (g) {
                sema_error_at(u, e->line, e->col,
                           "'%s' is used before its declaration "
                           "(line %d)", e->name, g->line);
            } else if (find_func(u, e->name)) {
                struct func *fd = find_func(u, e->name);
                /* seq-based, not the ordered-walk `declared` flag: a function
                 * used as a value in a static initializer (a vtable) is
                 * lowered before that walk runs, but is still legal if the
                 * function was declared earlier in the source. */
                if (fd->seq > cur_body_seq)
                    sema_error_at(u, e->line, e->col,
                               "'%s' is used before its declaration",
                               e->name);
                e->fref = fd;
                fd->used = 1;
                e->ty = ty_ptr(ty_func(fd->ret_ty, fd->param_tys,
                                       fd->nparams, fd->is_varargs));
                e->ty->pointee->sret_first = fd->sret_first;
            } else {
                /* A name misspelt once is usually used several times:
                 * report it once per function, then carry on quietly. */
                if (reported_undeclared(e->name)) {
                    g_sema_errors++;
                    if (g_recover)
                        longjmp(*g_recover, 1);
                    exit(1);
                }
                const char *sug = suggest_name(u, sc, e->name);
                const char *hdr = header_declaring(e->name);
                diag_error_at(diag_file(u), e->line, e->col,
                              "'%s' is not declared in '%s' — for a call, "
                              "add a prototype or define it first",
                              e->name, f->name);
                diag_set_id("E0001");
                if (hdr) {
                    /* The C library's own surface is fixed by the standard,
                     * so this is knowledge, not a guess: say which header,
                     * and offer to add it. */
                    diag_note_at(diag_file(u), 1, 1,
                                 "'%s' is declared in %s", e->name, hdr);
                    char inc[64];
                    snprintf(inc, sizeof inc, "#include %s\n", hdr);
                    diag_fixit_at(diag_file(u), 1, 1, 1, inc);
                }
                if (sug) {
                    diag_note_at(diag_file(u), e->line, e->col,
                                 "did you mean '%s'?", sug);
                    /* the edit itself, for whoever reads the JSON */
                    diag_fixit_at(diag_file(u), e->line, e->col,
                                  e->col + (int)strlen(e->name), sug);
                }
                g_sema_errors++;
                if (g_recover)
                    longjmp(*g_recover, 1);
                exit(1);
            }
        }
        if (e->ty->kind == TY_ARRAY) {
            e->undecayed = e->ty;
            e->ty = ty_ptr(e->ty->pointee);
        }
        break;
    }
    case EXPR_ASSIGN:
        check_expr(u, f, sc, e->lhs);
        if (!is_lvalue(e->lhs))
            sema_error_at(u, e->line, e->col, "assignment target is not an "
                                         "lvalue");
        if (e->lhs->undecayed)
            sema_error_at(u, e->line, e->col, "cannot assign to an array");
        if (e->lhs->fref)
            sema_error_at(u, e->line, e->col, "cannot assign to a function");
        check_expr(u, f, sc, e->rhs);
        if (ty_is_complex(e->lhs->ty) || ty_is_complex(e->rhs->ty)) {
            e->rhs = convert_assign(u, e->rhs, e->lhs->ty, "assignment");
        } else if (e->lhs->ty->kind == TY_STRUCT) {
            if (!ty_equal(e->lhs->ty, e->rhs->ty))
                sema_error_at(u, e->line, e->col,
                           "cannot assign %s to %s",
                           ty_name(e->rhs->ty), ty_name(e->lhs->ty));
        } else {
            need_scalar(u, e->rhs, "assignment");
            e->rhs = convert_assign(u, e->rhs, e->lhs->ty, "assignment");
        }
        e->ty = e->lhs->ty;
        break;
    case EXPR_INCDEC: {
        check_expr(u, f, sc, e->lhs);
        if (!is_lvalue(e->lhs) || e->lhs->undecayed || e->lhs->fref)
            sema_error_at(u, e->line, e->col, "++/-- needs an lvalue");
        e->ty = e->lhs->ty;
        if (ty_is_complex(e->ty) && cx_lowering()) {   /* z += 1 */
            struct expr *one = cx_node(EXPR_NUM, e->line, ty_base(TY_INT, 0));
            one->num = 1;
            *e = *cx_update(u, e->lhs, e->delta > 0 ? B_ADD : B_SUB, one,
                            e->is_post);
            break;
        }
        if (e->ty->kind == TY_PTR) {
            if (e->ty->pointee->kind == TY_VOID ||
                e->ty->pointee->kind == TY_FUNC)
                sema_error_at(u, e->line, e->col, "++/-- on %s",
                           ty_name(e->ty));
        } else if (!ty_is_arith(e->ty)) {
            sema_error_at(u, e->line, e->col, "++/-- needs an integer or "
                                         "pointer, got %s",
                       ty_name(e->ty));
        }
        break;
    }
    case EXPR_NOT:
        check_expr(u, f, sc, e->rhs);
        if (ty_is_complex(e->rhs->ty) && cx_lowering())
            e->rhs = cx_truth(e->rhs);
        else
            need_scalar(u, e->rhs, "'!'");
        e->ty = ty_base(TY_INT, 0);
        break;
    case EXPR_NEG:
        check_expr(u, f, sc, e->rhs);
        if (ty_is_complex(e->rhs->ty)) {
            if (!cx_lowering()) { e->ty = e->rhs->ty; break; }
            struct expr *re, *im;
            struct expr *pre = cx_parts(e->rhs, e->rhs->ty->celem, &re, &im);
            *e = *cx_comma(pre, cx_make(e->rhs->ty, cx_neg(re), cx_neg(im),
                                        e->line));
            break;
        }
        need_arith(u, e->rhs, "unary '-'");
        e->ty = promote(e->rhs->ty);
        e->rhs = mk_cast(e->rhs, e->ty);
        break;
    case EXPR_BNOT:
        check_expr(u, f, sc, e->rhs);
        if (ty_is_complex(e->rhs->ty)) {   /* GNU: ~z is the conjugate */
            if (!cx_lowering()) { e->ty = e->rhs->ty; break; }
            struct expr *re, *im;
            struct expr *pre = cx_parts(e->rhs, e->rhs->ty->celem, &re, &im);
            *e = *cx_comma(pre, cx_make(e->rhs->ty, re, cx_neg(im), e->line));
            break;
        }
        need_integer(u, e->rhs, "'~'");
        e->ty = promote(e->rhs->ty);
        e->rhs = mk_cast(e->rhs, e->ty);
        break;
    case EXPR_DEREF:
        check_expr(u, f, sc, e->rhs);
        if (e->rhs->ty->kind != TY_PTR)
            sema_error_at(u, e->line, e->col, "cannot dereference %s",
                       ty_name(e->rhs->ty));
        if (e->rhs->ty->pointee->kind == TY_FUNC) {
            e->ty = e->rhs->ty; /* *fp is fp, as in C */
            break;
        }
        if (e->rhs->ty->pointee->kind == TY_VOID)
            sema_error_at(u, e->line, e->col, "cannot dereference void *");
        e->ty = e->rhs->ty->pointee;
        if (e->ty->kind == TY_ARRAY) {
            /* m[i] of a 2-D array is itself an array: it decays, and
             * irgen "loads" it as its address, not its bytes. */
            e->undecayed = e->ty;
            e->ty = ty_ptr(e->ty->pointee);
        }
        break;
    case EXPR_LABELADDR:
        /* &&label: a void* to a code location (GNU computed goto). The label's
         * existence is resolved in irgen (labels may be forward-referenced). */
        e->ty = ty_ptr(ty_base(TY_VOID, 0));
        break;
    case EXPR_EHTYPEID:
        if (e->name) {
            e->gref = find_global(u, e->name);
            if (!e->gref)
                sema_error_at(u, e->line, e->col, "'%s' is not a declared "
                        "typeinfo object", e->name);
            e->gref->used = 1;
        }
        e->ty = ty_base(TY_LONG, 1);
        break;
    case EXPR_ADDR:
        check_expr(u, f, sc, e->rhs);
        if (e->rhs->fref) {
            *e = *e->rhs; /* &f is f: already a pointer-to-function */
            break;
        }
        if (!is_lvalue(e->rhs))
            sema_error_at(u, e->line, e->col,
                       "'&' needs a variable or *pointer");
        if (e->rhs->undecayed) {
            /* &arr yields a pointer to the whole ARRAY object, T(*)[N]; its
             * value is the array's address, which gen_addr already gives. */
            e->ty = ty_ptr(e->rhs->undecayed);
            break;
        }
        if (e->rhs->kind == EXPR_MEMBER && e->rhs->memb &&
            e->rhs->memb->is_bitfield)
            sema_error_at(u, e->line, e->col,
                       "cannot take the address of bitfield '%s'",
                       e->rhs->memb->name);
        e->ty = ty_ptr(e->rhs->ty);
        break;
    case EXPR_COMPLIT: {
        /* `(type){ init }` — an unnamed object with automatic storage in
         * this block. Give it a synthesized local slot and flatten the
         * initializer into it exactly like a declared aggregate; the
         * literal is an lvalue of that type (an array decays, a struct is
         * carried by address, a scalar is loaded). */
        struct type *ty = e->cast_ty;
        if (ty->kind == TY_VOID || ty->kind == TY_FUNC)
            sema_error_at(u, e->line, e->col,
                       "a compound literal cannot have type %s",
                       ty_name(ty));
        if (ty->kind == TY_STRUCT && !ty->complete)
            sema_error_at(u, e->line, e->col,
                       "compound literal of incomplete type %s",
                       ty_name(ty));
        /* `(int[]){...}` takes its size from the initializer, as `int a[]`
         * does. */
        if (ty->kind == TY_ARRAY && ty->count == 0 &&
            e->lhs->kind == EXPR_INITLIST)
            ty = ty_array(ty->pointee, initlist_array_count(e->lhs));
        e->cast_ty = ty;
        if (g_in_static_init) {
            /* Static storage: the literal is an anonymous global, and this
             * node becomes a reference to it (so `&(T){...}` in a static
             * initializer lowers to a relocation like any `&global`). */
            struct initbuf ib = { 0, 0, 0 };
            flatten_init(u, f, sc, e->lhs, ty, 0, &ib);
            static int anon_seq;
            struct global *g = xcalloc(1, sizeof *g);
            char *nm = xmalloc(24);
            snprintf(nm, 24, ".Lcomplit.%d", anon_seq++);
            g->name = nm;
            g->line = e->line;
            g->seq = -1;
            g->ty = ty;
            g->is_static = 1;
            g->defined = 1;
            g->used = 1;
            g->has_init = 1;   /* it has real bytes -> .data, not .bss */
            lower_static_bytes(u, e->line, ty_size(ty), ib.v, ib.n,
                               &g->init_bytes, &g->relocs, &g->nrelocs);
            g->init_len = ty_size(ty);
            struct global **gt = &u->globals;
            while (*gt) gt = &(*gt)->next;
            *gt = g;
            e->kind = EXPR_VAR;      /* an lvalue naming the anonymous global */
            e->gref = g;
            e->name = g->name;
            e->inits = NULL; e->ninits = 0; e->lhs = NULL;
            if (ty->kind == TY_ARRAY) {
                e->undecayed = ty;
                e->ty = ty_ptr(ty->pointee);
            } else {
                e->ty = ty;
            }
            break;
        }
        e->var_index = scope_add(sc, "<compound literal>", ty, NULL);
        struct initbuf ib = { 0, 0, 0 };
        flatten_init(u, f, sc, e->lhs, ty, 0, &ib);
        e->inits = ib.v;
        e->ninits = ib.n;
        e->lhs = NULL;
        if (ty->kind == TY_ARRAY) {
            e->undecayed = ty;   /* the object; e->ty is the decayed pointer */
            e->ty = ty_ptr(ty->pointee);
        } else {
            e->ty = ty;
        }
        break;
    }
    case EXPR_STMTEXPR: {
        /* GNU statement expression: check the block, then its value is the
         * last statement when that is an expression statement, else void.
         * The block shares the function's flat scope (as any block does). */
        check_stmt(u, f, sc, e->body, 0, 0, 0);
        struct stmt *last = NULL;
        for (struct stmt *s = e->body->body; s; s = s->next)
            last = s;
        e->ty = (last && last->kind == STMT_EXPR && last->expr)
              ? last->expr->ty : ty_base(TY_VOID, 0);
        break;
    }
    case EXPR_GENERIC: {
        /* Pick the association whose type matches the controlling
         * expression's (after its lvalue conversion — an array/function
         * operand already carries its decayed type here), else `default`.
         * The controlling expression is not evaluated (C11 6.5.1.1); the
         * node simply becomes the selected expression. */
        check_expr(u, f, sc, e->lhs);
        struct expr *chosen = NULL, *deflt = NULL;
        for (int i = 0; i < e->ngen; i++) {
            if (!e->gtypes[i]) { deflt = e->gexprs[i]; continue; }
            if (ty_equal(e->lhs->ty, e->gtypes[i])) {
                chosen = e->gexprs[i];
                break;
            }
        }
        if (!chosen)
            chosen = deflt;
        if (!chosen)
            sema_error_at(u, e->line, e->col,
                       "no _Generic association matches type %s",
                       ty_name(e->lhs->ty));
        check_expr(u, f, sc, chosen);
        *e = *chosen;   /* become the selected expression */
        break;
    }
    case EXPR_CAST:
        check_expr(u, f, sc, e->rhs);
        if (ty_is_complex(e->cast_ty) || ty_is_complex(e->rhs->ty)) {
            if (!ty_is_complex(e->rhs->ty) && !ty_is_arith(e->rhs->ty))
                sema_error_at(u, e->line, e->col, "cannot cast %s to %s",
                        ty_name(e->rhs->ty), ty_name(e->cast_ty));
            if (e->cast_ty->kind == TY_VOID) { e->ty = e->cast_ty; break; }
            *e = *cx_cast(e->rhs, e->cast_ty);
            break;
        }
        if (e->cast_ty->kind == TY_VOID) {
            /* (void)x — evaluate and discard, the standard way to say
             * "yes, I meant to ignore this" */
            e->ty = e->cast_ty;
            break;
        }
        need_scalar(u, e->rhs, "a cast");
        if (!ty_is_scalar(e->cast_ty))
            sema_error_at(u, e->line, e->col, "cannot cast to %s",
                       ty_name(e->cast_ty));
        if ((ty_is_float(e->cast_ty) && e->rhs->ty->kind == TY_PTR) ||
            (e->cast_ty->kind == TY_PTR && ty_is_float(e->rhs->ty)))
            sema_error_at(u, e->line, e->col,
                       "cannot convert between %s and %s",
                       ty_name(e->rhs->ty), ty_name(e->cast_ty));
        e->ty = e->cast_ty;
        break;
    case EXPR_COMMA:
        check_expr(u, f, sc, e->lhs); /* evaluated, value discarded */
        check_expr(u, f, sc, e->rhs);
        e->ty = e->rhs->ty;
        break;
    case EXPR_COND: {
        check_expr(u, f, sc, e->args[0]);
        if (ty_is_complex(e->args[0]->ty) && cx_lowering())
            e->args[0] = cx_truth(e->args[0]);
        else
            need_scalar(u, e->args[0], "'?:'");
        check_expr(u, f, sc, e->lhs);
        check_expr(u, f, sc, e->rhs);
        struct type *a = e->lhs->ty, *b = e->rhs->ty;
        if ((ty_is_complex(a) || ty_is_complex(b)) &&
            (ty_is_complex(a) || ty_is_arith(a)) &&
            (ty_is_complex(b) || ty_is_arith(b))) {
            e->ty = ty_complex(cx_common(a, b));
            e->lhs = cx_cast(e->lhs, e->ty);
            e->rhs = cx_cast(e->rhs, e->ty);
        } else if (ty_is_arith(a) && ty_is_arith(b)) {
            e->ty = arith_common(a, b);
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, e->ty);
        } else if (a->kind == TY_PTR && b->kind == TY_PTR) {
            if (!ty_equal(a, b) && a->pointee->kind != TY_VOID &&
                b->pointee->kind != TY_VOID)
                sema_error_at(u, e->line, e->col,
                           "'?:' branches have incompatible pointer "
                           "types (%s vs %s)", ty_name(a), ty_name(b));
            e->ty = a->pointee->kind == TY_VOID ? b : a;
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, e->ty);
        } else if (a->kind == TY_PTR && is_null_const(e->rhs)) {
            e->ty = a;
            e->rhs = mk_cast(e->rhs, a);
        } else if (b->kind == TY_PTR && is_null_const(e->lhs)) {
            e->ty = b;
            e->lhs = mk_cast(e->lhs, b);
        } else if (a->kind == TY_VOID && b->kind == TY_VOID) {
            e->ty = a;
        } else if (a->kind == TY_STRUCT && ty_equal(a, b)) {
            e->ty = a; /* both arms are the same aggregate */
        } else {
            sema_error_at(u, e->line, e->col,
                       "'?:' branches have incompatible types "
                       "(%s vs %s)", ty_name(a), ty_name(b));
        }
        break;
    }
    case EXPR_INITLIST:
        /* Only ever reached through flatten_init(), which knows the
         * target type; a brace list has no type of its own. */
        sema_error_at(u, e->line, e->col,
                   "a brace initializer cannot appear here");
        break;
    case EXPR_COMPOUND: {
        /* x op= y. The lvalue is evaluated once (irgen keeps its
         * address); the operation happens in the usual common type and
         * the result converts back to the target's type, as C says. */
        check_expr(u, f, sc, e->lhs);
        check_expr(u, f, sc, e->rhs);
        if (!is_lvalue(e->lhs) || e->lhs->undecayed || e->lhs->fref)
            sema_error_at(u, e->line, e->col,
                       "compound assignment needs an lvalue");
        if ((ty_is_complex(e->lhs->ty) || ty_is_complex(e->rhs->ty)) &&
            cx_lowering()) {
            *e = *cx_update(u, e->lhs, e->op, e->rhs, 0);
            break;
        }
        if (e->lhs->ty->kind == TY_PTR) {
            if (e->op != B_ADD && e->op != B_SUB)
                sema_error_at(u, e->line, e->col,
                           "only += and -= apply to a pointer");
            need_integer(u, e->rhs, "pointer arithmetic");
            e->rhs = mk_cast(e->rhs, ty_base(TY_LONG, 0));
            e->cast_ty = e->lhs->ty;
            e->ty = e->lhs->ty;
            break;
        }
        if (e->op == B_SHL || e->op == B_SHR) {
            need_integer(u, e->lhs, "a shift");
            need_integer(u, e->rhs, "a shift");
            e->cast_ty = promote(e->lhs->ty);
            e->rhs = mk_cast(e->rhs, ty_base(TY_INT, 0));
            e->ty = e->lhs->ty;
            break;
        }
        if (e->op == B_ADD || e->op == B_SUB || e->op == B_MUL ||
            e->op == B_DIV) {
            need_arith(u, e->lhs, "compound assignment");
            need_arith(u, e->rhs, "compound assignment");
        } else {
            need_integer(u, e->lhs, "this operator");
            need_integer(u, e->rhs, "this operator");
        }
        e->cast_ty = arith_common(e->lhs->ty, e->rhs->ty);
        e->rhs = mk_cast(e->rhs, e->cast_ty);
        e->ty = e->lhs->ty;
        break;
    }
    case EXPR_MEMBER: {
        check_expr(u, f, sc, e->lhs);
        struct type *base = e->lhs->ty;
        if (e->is_arrow) {
            if (base->kind != TY_PTR || base->pointee->kind != TY_STRUCT) {
                /* `->` on a value of struct type: the edit is a '.' */
                int fixable = base->kind == TY_STRUCT;
                diag_error_at(diag_file(u), e->line, e->col,
                              "'->' needs a pointer to a struct/union, "
                              "got %s%s", ty_name(base),
                              fixable ? " (use '.' on a value)" : "");
                if (fixable) {
                    diag_note_at(diag_file(u), e->line, e->col,
                                 "use '.' here");
                    diag_fixit_find(diag_file(u), e->line, e->col, "->", ".");
                }
                g_sema_errors++;
                if (g_recover)
                    longjmp(*g_recover, 1);
                exit(1);
            }
            base = base->pointee;
        } else if (base->kind != TY_STRUCT) {
            int fixable = base->kind == TY_PTR &&
                          base->pointee->kind == TY_STRUCT;
            diag_error_at(diag_file(u), e->line, e->col,
                          "'.' needs a struct/union, got %s%s", ty_name(base),
                          fixable ? " (use '->' through a pointer)" : "");
            if (fixable) {
                diag_note_at(diag_file(u), e->line, e->col, "use '->' here");
                diag_fixit_find(diag_file(u), e->line, e->col, ".", "->");
            }
            g_sema_errors++;
            if (g_recover)
                longjmp(*g_recover, 1);
            exit(1);
        }
        if (!base->complete)
            sema_error_at(u, e->line, e->col,
                       "%s is incomplete here (its body comes later "
                       "or never)", ty_name(base));
        struct member *mm = xcalloc(1, sizeof *mm);
        if (!find_member_deep(base, e->name, mm, 0)) {
            /* The member it is nearest to, among the ones this type has:
             * a suggestion from the type in hand, not from a dictionary. */
            const char *best = NULL;
            int bestd = 1000, nlen = (int)strlen(e->name);
            for (int mi = 0; mi < base->nmembers; mi++) {
                const char *mn = base->members[mi].name;
                if (!mn)
                    continue;
                int d = edit_distance(e->name, mn);
                if (d > 0 && d < bestd) { bestd = d; best = mn; }
            }
            int thresh = nlen / 3 < 2 ? 2 : nlen / 3;
            diag_error_at(diag_file(u), e->line, e->col,
                          "%s has no member '%s'", ty_name(base), e->name);
            diag_set_id("E0004");
            if (best && bestd <= thresh) {
                diag_note_at(diag_file(u), e->line, e->col,
                             "did you mean '%s'?", best);
                diag_fixit_find(diag_file(u), e->line, e->col, e->name, best);
            }
            g_sema_errors++;
            if (g_recover)
                longjmp(*g_recover, 1);
            exit(1);
        }
        e->memb = mm;
        e->ty = e->memb->ty;
        /* C: a member of a `volatile`-qualified struct/union is itself
         * volatile-qualified — propagate it so the load/store isn't optimized
         * (the ehci/ohci MMIO register-struct pattern), and so a nested struct
         * member stays volatile for its own members. */
        if (base->is_volatile && !e->ty->is_volatile)
            e->ty = ty_volatile(e->ty);
        if (e->ty->kind == TY_ARRAY) {
            e->undecayed = e->ty;
            e->ty = ty_ptr(e->ty->pointee);
        }
        break;
    }
    case EXPR_SIZEOF: {
        long size;
        /* sizeof a VLA is a run-time value: irgen reads its size slot (for
         * a type name, after computing it). Stays EXPR_SIZEOF. The operand
         * EXPRESSION is not evaluated even so — C evaluates it only when
         * its own type is a VLA, which EmbCC does not do (yet). */
        if (e->cast_ty && ty_is_vm(e->cast_ty)) {
            vla_prepare(u, f, sc, e->cast_ty);
            if (ty_is_vla(e->cast_ty)) {
                e->ty = ty_base(TY_LONG, 1);
                break;
            }
        }
        if (!e->cast_ty) {
            check_expr(u, f, sc, e->rhs);
            struct type *rt = e->rhs->undecayed ? e->rhs->undecayed
                                                : e->rhs->ty;
            if (ty_is_vla(rt)) {
                e->ty = ty_base(TY_LONG, 1);
                break;
            }
        }
        if (e->cast_ty) {
            if (e->cast_ty->kind == TY_VOID)
                sema_error_at(u, e->line, e->col, "sizeof(void)");
            if (ty_size(e->cast_ty) == 0)
                sema_error_at(u, e->line, e->col, "sizeof of incomplete %s",
                           ty_name(e->cast_ty));
            size = ty_size(e->cast_ty);
        } else {
            if (e->rhs->ty->kind == TY_VOID)
                sema_error_at(u, e->line, e->col, "sizeof a void expression");
            /* sizeof is the one context where an array does NOT decay */
            size = ty_size(e->rhs->undecayed ? e->rhs->undecayed
                                             : e->rhs->ty);
        }
        /* Folded to a constant here; the operand is never evaluated,
         * exactly as C specifies. size_t is unsigned long in LP64. */
        e->kind = EXPR_NUM;
        e->num = size;
        e->rhs = NULL;
        e->ty = ty_base(TY_LONG, 1);
        break;
    }
    case EXPR_ALIGNOF: {
        struct type *t = e->cast_ty;
        if (!t) {
            check_expr(u, f, sc, e->rhs);
            t = e->rhs->undecayed ? e->rhs->undecayed : e->rhs->ty;
        }
        if (t->kind == TY_VOID || (ty_size(t) == 0 && !ty_is_vla(t)))
            sema_error_at(u, e->line, e->col, "_Alignof of incomplete %s",
                    ty_name(t));
        e->kind = EXPR_NUM;                 /* folds to a size_t constant */
        e->num = ty_align(t);
        e->rhs = NULL;
        e->ty = ty_base(TY_LONG, 1);
        break;
    }
    case EXPR_VA_ARG:
        check_expr(u, f, sc, e->lhs);   /* the va_list */
        if (e->cast_ty->kind == TY_VOID)
            sema_error_at(u, e->line, e->col, "va_arg cannot read type 'void'");
        if (e->cast_ty->kind == TY_STRUCT)
            sema_error_at(u, e->line, e->col,
                       "va_arg of a struct passed by value is not "
                       "supported yet");
        e->ty = e->cast_ty;
        break;
    case EXPR_BINOP: {
        check_expr(u, f, sc, e->lhs);
        check_expr(u, f, sc, e->rhs);
        struct type *lt = e->lhs->ty, *rt = e->rhs->ty;
        if (ty_is_complex(lt) || ty_is_complex(rt)) {
            cx_binop(u, e);
            break;
        }

        switch (e->op) {
        case B_LAND:
        case B_LOR:
            need_scalar(u, e->lhs, "'&&'/'||'");
            need_scalar(u, e->rhs, "'&&'/'||'");
            e->ty = ty_base(TY_INT, 0);
            break;
        case B_ADD:
        case B_SUB: {
            int lp = lt->kind == TY_PTR, rp = rt->kind == TY_PTR;
            if (lp && rp) {
                if (e->op == B_ADD)
                    sema_error_at(u, e->line, e->col,
                               "cannot add two pointers");
                if (!ty_equal(lt, rt))
                    sema_error_at(u, e->line, e->col,
                               "subtracting incompatible pointers "
                               "(%s vs %s)", ty_name(lt), ty_name(rt));
                if (lt->pointee->kind == TY_VOID ||
                    lt->pointee->kind == TY_FUNC)
                    sema_error_at(u, e->line, e->col, "arithmetic on %s",
                               ty_name(lt));
                e->ty = ty_base(TY_LONG, 0); /* ptrdiff_t */
            } else if (lp || rp) {
                if (rp && e->op == B_SUB)
                    sema_error_at(u, e->line, e->col,
                               "cannot subtract a pointer from an "
                               "integer");
                struct expr **ip = lp ? &e->rhs : &e->lhs;
                struct type *pt = lp ? lt : rt;
                if (pt->pointee->kind == TY_VOID ||
                    pt->pointee->kind == TY_FUNC)
                    sema_error_at(u, e->line, e->col, "arithmetic on %s",
                               ty_name(pt));
                need_integer(u, *ip, "pointer arithmetic");
                *ip = mk_cast(*ip, ty_base(TY_LONG, 0));
                e->ty = pt;
            } else {
                need_arith(u, e->lhs, "arithmetic");
                need_arith(u, e->rhs, "arithmetic");
                e->ty = arith_common(lt, rt);
                e->lhs = mk_cast(e->lhs, e->ty);
                e->rhs = mk_cast(e->rhs, e->ty);
            }
            break;
        }
        case B_EQ:
        case B_NE:
        case B_LT:
        case B_LE:
        case B_GT:
        case B_GE: {
            int lp = lt->kind == TY_PTR, rp = rt->kind == TY_PTR;
            if (lp || rp) {
                if (lp && rp) {
                    if (!ty_equal(lt, rt) &&
                        lt->pointee->kind != TY_VOID &&
                        rt->pointee->kind != TY_VOID)
                        sema_error_at(u, e->line, e->col,
                                   "comparing incompatible pointers "
                                   "(%s vs %s)", ty_name(lt),
                                   ty_name(rt));
                } else {
                    struct expr **ip = lp ? &e->rhs : &e->lhs;
                    if (!is_null_const(*ip))
                        sema_error_at(u, e->line, e->col,
                                   "comparing a pointer with an "
                                   "integer needs a cast");
                    *ip = mk_cast(*ip, lp ? lt : rt);
                }
            } else {
                need_arith(u, e->lhs, "comparison");
                need_arith(u, e->rhs, "comparison");
                struct type *ct = arith_common(lt, rt);
                /* -Wsign-compare: the conversion the standard prescribes
                 * turns the signed side unsigned, so a negative value
                 * compares as a very large one. Only worth saying when the
                 * signed side really can be negative — a constant that is
                 * not is converted at compile time and means what it
                 * says. */
                if (ty_is_integer(lt) && ty_is_integer(rt) &&
                    ty_signed_int(lt) != ty_signed_int(rt) &&
                    !ty_signed_int(ct)) {
                    struct expr *se = ty_signed_int(lt) ? e->lhs : e->rhs;
                    long v;
                    if (!(const_fold(se, &v) && v >= 0))
                        diag_warn_opt(diag_file(u), e->line, e->col, "sign-compare",
                                      "comparison between %s and %s",
                                      ty_name(lt), ty_name(rt));
                }
                e->lhs = mk_cast(e->lhs, ct);
                e->rhs = mk_cast(e->rhs, ct);
            }
            e->ty = ty_base(TY_INT, 0);
            break;
        }
        case B_SHL:
        case B_SHR:
            need_integer(u, e->lhs, "shift");
            need_integer(u, e->rhs, "shift");
            e->ty = promote(lt);
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, ty_base(TY_INT, 0));
            break;
        case B_MUL:
        case B_DIV:
            need_arith(u, e->lhs, "arithmetic");
            need_arith(u, e->rhs, "arithmetic");
            e->ty = arith_common(lt, rt);
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, e->ty);
            break;
        default: /* MOD AND OR XOR — integers only, as in C */
            need_integer(u, e->lhs, "this operator");
            need_integer(u, e->rhs, "this operator");
            e->ty = arith_common(lt, rt);
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, e->ty);
            break;
        }
        break;
    }
    case EXPR_CALL: {
        /* The stdarg builtins are not real functions: va_start needs the
         * ADDRESS of its va_list (irgen takes it), and none is declared
         * anywhere. Type-check the operands, mark the enclosing function
         * variadic, and hand back void. va_end is a no-op. va_copy gets a
         * hidden 32-byte slot: a va_list is a pointer to the va_start-built
         * tag, which va_arg advances in place, so a copy needs a tag of its
         * own for the destination to point at. */
        if (e->lhs->kind == EXPR_VAR && e->lhs->name &&
            (strcmp(e->lhs->name, "__builtin_va_start") == 0 ||
             strcmp(e->lhs->name, "__builtin_va_copy") == 0 ||
             strcmp(e->lhs->name, "__builtin_va_end") == 0)) {
            int is_start = strcmp(e->lhs->name, "__builtin_va_start") == 0;
            int is_copy = strcmp(e->lhs->name, "__builtin_va_copy") == 0;
            int want = is_start || is_copy ? 2 : 1;
            if (e->nargs != want)
                sema_error_at(u, e->line, e->col,
                           "%s takes %d argument%s", e->lhs->name, want,
                           want == 1 ? "" : "s");
            for (int i = 0; i < e->nargs; i++)
                check_expr(u, f, sc, e->args[i]);
            if (!is_lvalue(e->args[0]))
                sema_error_at(u, e->line, e->col,
                           "the first argument to %s must be a va_list "
                           "variable", e->lhs->name);
            if (is_start && !f->is_varargs)
                sema_error_at(u, e->line, e->col,
                           "va_start in '%s', which is not variadic",
                           f->name);
            if (is_copy)   /* SysV's tag is 24 bytes, AAPCS64's 32 */
                e->var_index = scope_add(sc, "<va_copy tag>",
                                         ty_array(ty_base(TY_LONG, 0), 4),
                                         NULL);
            e->name = e->lhs->name;   /* irgen dispatches on it */
            e->ty = ty_base(TY_VOID, 0);
            break;
        }
        /* Compiler builtins for the floating specials: lower directly to a
         * constant with the right bit pattern (HUGE_VAL/INFINITY/NAN expand to
         * these). The `f` variants are single precision; nan's string arg is
         * evaluated for side effects only (there are none) and ignored. */
        if (e->lhs->kind == EXPR_VAR && e->lhs->name &&
            strncmp(e->lhs->name, "__builtin_", 10) == 0) {
            const char *bn = e->lhs->name + 10;
            int is_inf = strcmp(bn, "inf") == 0 || strcmp(bn, "huge_val") == 0;
            int is_inff = strcmp(bn, "inff") == 0 || strcmp(bn, "huge_valf") == 0;
            int is_nan = strcmp(bn, "nan") == 0;
            int is_nanf = strcmp(bn, "nanf") == 0;
            int is_infl = strcmp(bn, "infl") == 0 || strcmp(bn, "huge_vall") == 0;
            int is_nanl = strcmp(bn, "nanl") == 0;
            if (is_inf || is_inff || is_nan || is_nanf || is_infl || is_nanl) {
                for (int i = 0; i < e->nargs; i++)
                    check_expr(u, f, sc, e->args[i]);
                e->kind = EXPR_FNUM;
                e->fnum = (is_nan || is_nanf || is_nanl) ? ieee_nan()
                                                         : ieee_inf();
                e->ty = ty_base((is_inff || is_nanf) ? TY_FLOAT : TY_DOUBLE, 0);
                if (is_infl || is_nanl) {
                    e->ty = ty_base(TY_LDOUBLE, 0);
                    e->ldv = ldf_from_double(e->fnum);   /* inf/nan exactly */
                }
                break;
            }
            /* __builtin_alloca(n): n bytes on this function's stack (16-
             * aligned), freed when it returns — as a VLA's storage is
             * carved, without a scope of its own. */
            if (strcmp(bn, "alloca") == 0 ||
                strcmp(bn, "alloca_with_align") == 0) {
                if (e->nargs < 1)
                    sema_error_at(u, e->line, e->col, "%s takes a size",
                            e->lhs->name);
                for (int i = 0; i < e->nargs; i++)
                    check_expr(u, f, sc, e->args[i]);
                e->args[0] = mk_cast(e->args[0], ty_base(TY_LONG, 1));
                e->nargs = 1;
                e->name = "__builtin_alloca";
                e->ty = ty_ptr(ty_base(TY_VOID, 0));
                break;
            }
            /* __builtin_return_address(N) / __builtin_frame_address(N): the
             * frame chain, which EmbCC always keeps (both targets lay a frame
             * out as [saved frame pointer][return address]). N must be a
             * constant, as gcc requires; the result is void *. */
            if (strcmp(bn, "return_address") == 0 ||
                strcmp(bn, "frame_address") == 0) {
                long lvl;
                if (e->nargs != 1)
                    sema_error_at(u, e->line, e->col, "%s takes one argument",
                            e->lhs->name);
                check_expr(u, f, sc, e->args[0]);
                if (!const_fold(e->args[0], &lvl) || lvl < 0)
                    sema_error_at(u, e->line, e->col,
                            "%s needs a non-negative constant level",
                            e->lhs->name);
                e->name = e->lhs->name;
                e->num = lvl;
                e->ty = ty_ptr(ty_base(TY_VOID, 0));
                break;
            }
            /* __builtin_memcpy/memmove/memset are the libc functions under a
             * reserved name -- rename and let the ordinary call path resolve
             * them (the program must declare/provide them). */
            if (strcmp(bn, "memcpy") == 0 || strcmp(bn, "memmove") == 0 ||
                strcmp(bn, "memset") == 0) {
                e->lhs->name = bn;   /* fall through to normal call handling */
                implicit_mem_decl(u, bn);
            }
            /* byte swaps -> a single instruction; the result is the argument's
             * width as an unsigned integer. */
            else if (strcmp(bn, "bswap16") == 0 || strcmp(bn, "bswap32") == 0 ||
                     strcmp(bn, "bswap64") == 0) {
                if (e->nargs != 1)
                    sema_error_at(u, e->line, e->col, "%s takes one argument",
                               e->lhs->name);
                check_expr(u, f, sc, e->args[0]);
                need_integer(u, e->args[0], "__builtin_bswap");
                e->name = e->lhs->name;
                e->ty = ty_base(bn[5] == '1' ? TY_SHORT :
                                bn[5] == '3' ? TY_INT : TY_LONG, 1);
                break;
            }
            /* the value IS the first argument; the hint is discarded */
            else if (strcmp(bn, "expect") == 0 ||
                     strcmp(bn, "expect_with_probability") == 0 ||
                     strcmp(bn, "assume_aligned") == 0) {
                /* hints: the value is the first argument, unchanged */
                if (e->nargs < 2)
                    sema_error_at(u, e->line, e->col,
                               "%s takes at least two arguments", e->lhs->name);
                for (int i = 0; i < e->nargs; i++)
                    check_expr(u, f, sc, e->args[i]);
                *e = *e->args[0];
                break;
            }
            /* __builtin_constant_p: is the argument a compile-time constant?
             * Answered here, and the argument is never evaluated. */
            else if (strcmp(bn, "constant_p") == 0) {
                long cv;
                if (e->nargs != 1)
                    sema_error_at(u, e->line, e->col,
                            "__builtin_constant_p takes one argument");
                check_expr(u, f, sc, e->args[0]);
                int line = e->line, col = e->col;
                int k = const_fold(e->args[0], &cv);
                memset(e, 0, sizeof *e);
                e->kind = EXPR_NUM;
                e->line = line;
                e->col = col;
                e->num = k;
                e->ty = ty_base(TY_INT, 0);
                break;
            }
            /* __builtin_prefetch: a hint with no effect here, but the address
             * is still evaluated for its side effects. */
            else if (strcmp(bn, "prefetch") == 0) {
                if (e->nargs < 1)
                    sema_error_at(u, e->line, e->col,
                            "__builtin_prefetch takes an address");
                for (int i = 0; i < e->nargs; i++)
                    check_expr(u, f, sc, e->args[i]);
                e->name = e->lhs->name;
                e->ty = ty_base(TY_VOID, 0);
                break;
            }
            /* The bit family: ctz clz popcount ffs parity clrsb, each with l
             * and ll forms taking long / long long. Lowered in irgen from
             * ordinary integer ops, on both targets alike. */
            else if (builtin_bitop(bn, NULL) != 0) {
                if (e->nargs != 1)
                    sema_error_at(u, e->line, e->col, "%s takes one argument",
                            e->lhs->name);
                check_expr(u, f, sc, e->args[0]);
                need_integer(u, e->args[0], e->lhs->name);
                e->name = e->lhs->name;
                e->ty = ty_base(TY_INT, 0);
                break;
            }
            /* control never reaches here -> a trap (ud2 / udf) */
            else if (strcmp(bn, "unreachable") == 0 || strcmp(bn, "trap") == 0) {
                e->name = e->lhs->name;
                e->ty = ty_base(TY_VOID, 0);
                break;
            }
        }
        /* a full memory barrier (not spelled __builtin_) */
        if (e->lhs->kind == EXPR_VAR && e->lhs->name &&
            strcmp(e->lhs->name, "__sync_synchronize") == 0) {
            e->name = e->lhs->name;
            e->ty = ty_base(TY_VOID, 0);
            break;
        }
        /* The GCC atomic builtins, typed here and lowered in irgen. */
        if (e->lhs->kind == EXPR_VAR && e->lhs->name) {
            int aop;
            enum atomic_kind ak = atomic_builtin(e->lhs->name, &aop);
            if (ak != AK_NONE) {
                check_atomic_call(u, f, sc, e, ak);
                break;
            }
        }
        /* Direct when the callee is a name that is not a variable in
         * scope and names a function; otherwise a call through a
         * function-pointer value. */
        struct type *ft = NULL;
        e->callee = NULL;
        if (e->lhs->kind == EXPR_VAR &&
            scope_find(sc, e->lhs->name) < 0 &&
            !find_global(u, e->lhs->name) &&
            find_func(u, e->lhs->name)) {
            struct func *callee = find_func(u, e->lhs->name);
            if (callee->seq > cur_body_seq)
                sema_error_at(u, e->line, e->col,
                           "call to '%s' before its declaration — "
                           "declare or define functions before their "
                           "callers", e->lhs->name);
            e->callee = callee;
            callee->used = 1;
            ft = ty_func(callee->ret_ty, callee->param_tys,
                         callee->nparams, callee->is_varargs);
            ft->sret_first = callee->sret_first;
        } else {
            check_expr(u, f, sc, e->lhs);
            if (e->lhs->ty->kind != TY_PTR ||
                e->lhs->ty->pointee->kind != TY_FUNC)
                sema_error_at(u, e->line, e->col,
                           "called object is not a function (type %s)",
                           ty_name(e->lhs->ty));
            ft = e->lhs->ty->pointee;
        }
        if (ft->is_varargs ? e->nargs < ft->nptypes
                           : e->nargs != ft->nptypes)
            sema_error_at(u, e->line, e->col,
                       "this call needs %s%d argument%s, got %d",
                       ft->is_varargs ? "at least " : "", ft->nptypes,
                       ft->nptypes == 1 ? "" : "s", e->nargs);
        for (int i = 0; i < e->nargs; i++) {
            check_expr(u, f, sc, e->args[i]);
            if (e->args[i]->ty->kind != TY_STRUCT)
                need_scalar(u, e->args[i], "an argument");
            if (i < ft->nptypes)
                e->args[i] = convert_assign(u, e->args[i],
                                            ft->ptypes[i], "argument");
            else /* variadic tail: default argument promotions */
                e->args[i] = mk_cast(e->args[i],
                                     default_arg_promote(e->args[i]->ty));
        }
        e->ty = ft->ret;
        break;
    }
    }
}

/* Enough constant folding for a case label. Anything it cannot fold is
 * refused by name rather than guessed at — case labels must be integer
 * constant expressions, and a label we cannot evaluate is a label we
 * cannot dispatch on (THE RULE). */
static int const_fold(const struct expr *e, long *out)
{
    long a, b;

    switch (e->kind) {
    case EXPR_NUM:
        *out = e->num;
        return 1;
    case EXPR_CAST:
        return const_fold(e->rhs, out);
    case EXPR_NEG:
        if (!const_fold(e->rhs, &a))
            return 0;
        *out = -a;
        return 1;
    case EXPR_BNOT:
        if (!const_fold(e->rhs, &a))
            return 0;
        *out = ~a;
        return 1;
    case EXPR_NOT:
        if (!const_fold(e->rhs, &a))
            return 0;
        *out = !a;
        return 1;
    case EXPR_BINOP:
        if (!const_fold(e->lhs, &a) || !const_fold(e->rhs, &b))
            return 0;
        switch (e->op) {
        case B_ADD: *out = a + b; return 1;
        case B_SUB: *out = a - b; return 1;
        case B_MUL: *out = a * b; return 1;
        case B_DIV: if (!b) return 0; *out = a / b; return 1;
        case B_MOD: if (!b) return 0; *out = a % b; return 1;
        case B_AND: *out = a & b; return 1;
        case B_OR:  *out = a | b; return 1;
        case B_XOR: *out = a ^ b; return 1;
        case B_SHL: *out = a << b; return 1;
        case B_SHR: *out = a >> b; return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

/* e, a constant integer expression, as 128 bits (its own type's value,
 * extended per its signedness) */
static int const_fold128(const struct expr *e, struct w128 *out)
{
    if (!e->ty || e->ty->kind != TY_INT128) {
        long v;
        if (!e->ty || !ty_is_integer(e->ty) || !const_fold(e, &v))
            return 0;
        *out = w_make((unsigned long)v,
                      !e->ty->is_unsigned && v < 0 ? ~0UL : 0);
        return 1;
    }
    struct w128 a, b;
    int sign = !e->ty->is_unsigned;
    switch (e->kind) {
    case EXPR_NUM:
        *out = w_make((unsigned long)e->num, e->num < 0 ? ~0UL : 0);
        return 1;
    case EXPR_CAST:
        return const_fold128(e->rhs, out);
    case EXPR_NEG:
        if (!const_fold128(e->rhs, &a))
            return 0;
        *out = w_neg(a);
        return 1;
    case EXPR_BNOT:
        if (!const_fold128(e->rhs, &a))
            return 0;
        *out = w_make(~a.lo, ~a.hi);
        return 1;
    case EXPR_BINOP:
        if (!const_fold128(e->lhs, &a) || !const_fold128(e->rhs, &b))
            return 0;
        switch (e->op) {
        case B_ADD: *out = w_add(a, b); return 1;
        case B_SUB: *out = w_add(a, w_neg(b)); return 1;
        case B_MUL: *out = w_mul(a, b); return 1;
        case B_AND: *out = w_make(a.lo & b.lo, a.hi & b.hi); return 1;
        case B_OR:  *out = w_make(a.lo | b.lo, a.hi | b.hi); return 1;
        case B_XOR: *out = w_make(a.lo ^ b.lo, a.hi ^ b.hi); return 1;
        case B_SHL: *out = w_shl(a, (int)b.lo); return 1;
        case B_SHR: *out = w_shr(a, (int)b.lo, sign); return 1;
        case B_DIV: case B_MOD:
            return w_div(a, b, sign, e->op == B_MOD, out);
        default:
            return 0;
        }
    default:
        return 0;
    }
}

/* Fold a FLOATING constant expression to a double, for static initializers of
 * float/double storage (`double g = 1.0/3.0;`). Integer leaves promote to
 * double; a cast to an integer type truncates (C semantics), a cast to float
 * rounds to single precision. Returns 0 (not constant) if it can't reduce. */
static struct ldf *const_fold_ld(const struct expr *e);

static int const_fold_f(const struct expr *e, double *out)
{
    double a, b; long iv;
    /* a long double subexpression folds exactly, then narrows ONCE (going
     * through a host double first would round twice) */
    if (e->ty && e->ty->kind == TY_LDOUBLE) {
        struct ldf *x = const_fold_ld(e);
        if (!x) return 0;
        *out = ldf_to_double(x);
        return 1;
    }
    if (e->kind == EXPR_CAST && ty_is_float(e->ty) && e->rhs->ty &&
        e->rhs->ty->kind == TY_LDOUBLE) {
        struct ldf *x = const_fold_ld(e->rhs);
        if (!x) return 0;
        *out = ldf_to_double(ldf_round(x, e->ty->kind == TY_FLOAT ? LDF_FLOAT
                                                                  : LDF_DOUBLE));
        return 1;
    }
    switch (e->kind) {
    case EXPR_FNUM:
        *out = e->fnum;
        return 1;
    case EXPR_NUM:
        *out = (double)e->num;      /* an integer constant used where a float is wanted */
        return 1;
    case EXPR_CAST:
        if (ty_is_float(e->ty)) {
            if (!const_fold_f(e->rhs, &a)) return 0;
            *out = (e->ty->kind == TY_FLOAT) ? (double)(float)a : a;
            return 1;
        }
        if (ty_is_integer(e->ty)) {   /* (int)f : evaluate then truncate toward zero */
            if (const_fold_f(e->rhs, &a)) { *out = (double)(long)a; return 1; }
            if (const_fold(e->rhs, &iv)) { *out = (double)iv; return 1; }
        }
        return 0;
    case EXPR_NEG:
        if (!const_fold_f(e->rhs, &a)) return 0;
        *out = -a;
        return 1;
    case EXPR_BINOP:
        if (!const_fold_f(e->lhs, &a) || !const_fold_f(e->rhs, &b))
            return 0;
        switch (e->op) {
        case B_ADD: *out = a + b; return 1;
        case B_SUB: *out = a - b; return 1;
        case B_MUL: *out = a * b; return 1;
        case B_DIV: *out = a / b; return 1;   /* x/0.0 is inf/nan -- valid float result */
        default: return 0;
        }
    default:
        return 0;
    }
}

/* Fold a constant expression of type long double exactly, in the target's
 * format (sema/ldfloat.h): each operation rounds as the target's would, and
 * a double or float operand widens exactly from ITS OWN folded value.
 * NULL if it is not constant. */
static struct ldf *const_fold_ld(const struct expr *e)
{
    enum ldf_fmt fmt = ldf_target_fmt();
    struct ldf *a, *b;
    double d;
    long iv;
    switch (e->kind) {
    case EXPR_FNUM:
        if (e->ldv) return e->ldv;
        return ldf_from_double(e->ty && e->ty->kind == TY_FLOAT
                                   ? (double)(float)e->fnum : e->fnum);
    case EXPR_NUM:
        return ldf_round(ldf_from_int(e->num, e->ty && e->ty->is_unsigned), fmt);
    case EXPR_CAST: {
        const struct type *from = e->rhs->ty;
        if (from && from->kind == TY_LDOUBLE)
            return const_fold_ld(e->rhs);
        if (from && ty_is_float(from)) {
            if (!const_fold_f(e->rhs, &d)) return NULL;
            if (from->kind == TY_FLOAT) d = (double)(float)d;
            return ldf_from_double(d);
        }
        if (!const_fold(e->rhs, &iv)) return NULL;
        return ldf_round(ldf_from_int(iv, from && from->is_unsigned), fmt);
    }
    case EXPR_NEG:
        a = const_fold_ld(e->rhs);
        return a ? ldf_neg(a) : NULL;
    case EXPR_BINOP:
        if (e->op != B_ADD && e->op != B_SUB && e->op != B_MUL && e->op != B_DIV)
            return NULL;
        a = const_fold_ld(e->lhs);
        b = a ? const_fold_ld(e->rhs) : NULL;
        if (!b) return NULL;
        return ldf_binop(e->op == B_ADD ? '+' : e->op == B_SUB ? '-'
                         : e->op == B_MUL ? '*' : '/', a, b, fmt);
    default:
        return NULL;
    }
}

/* The statement list a switch dispatches over: its body, unwrapped when
 * it is the usual brace block. Case markers must live at THIS level. */
struct stmt *switch_stmts(struct stmt *body)
{
    if (body && body->kind == STMT_BLOCK)
        return body->body;
    return body;
}

/* Flattens an initializer against its target type into (offset, type,
 * value) triples. Nested braces recurse; a scalar initializer for an
 * aggregate member is checked and converted like any assignment. C's
 * rule that unlisted elements are zero is honoured by irgen, which
 * clears the whole object first. */
static void init_push(struct initbuf *b, int off, struct type *ty,
                      struct expr *e)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->v = xrealloc(b->v, (size_t)b->cap * sizeof *b->v);
    }
    b->v[b->n].off = off;
    b->v[b->n].ty = ty;
    b->v[b->n].e = e;
    b->v[b->n].bit_off = 0;
    b->v[b->n].bit_width = 0;
    b->v[b->n].bf_bytes = 0;
    b->n++;
}

/* A bitfield leaf: same as init_push but recording where in the storage
 * unit at `off` the value lands, so the lowerings mask and merge it. */
static void init_push_bf(struct initbuf *b, int off, struct type *ty,
                         struct expr *e, int bit_off, int bit_width,
                         int bf_bytes)
{
    init_push(b, off, ty, e);
    b->v[b->n - 1].bit_off = bit_off;
    b->v[b->n - 1].bit_width = bit_width;
    b->v[b->n - 1].bf_bytes = bf_bytes;
}

static void flatten_init(struct unit *u, struct func *f, struct scope *sc,
                         struct expr *init, struct type *ty, int off,
                         struct initbuf *out)
{
    /* A compound literal used AS an initializer is exactly its brace
     * initializer placed at this offset — `T g = (T){...}` (even at file
     * scope) and a literal nested in another initializer need no separate
     * object. Only an unbraced literal reaches here as `init` (a plain
     * `expr` initializer keeps EXPR_COMPLIT); `&(T){...}` stays an
     * EXPR_ADDR and is lowered through the anonymous-object path instead. */
    if (init->kind == EXPR_COMPLIT) {
        if (init->lhs && init->lhs->kind == EXPR_INITLIST) {
            flatten_init(u, f, sc, init->lhs, ty, off, out);
            return;
        }
    }
    if (init->kind != EXPR_INITLIST) {
        if (ty->kind == TY_ARRAY) {
            /* char a[] = "..." (or a wide array from L""/u""/U"") — the
             * literal's elements ARE the object. The array element must be an
             * integer whose size matches the literal's element width. */
            if (init->kind == EXPR_STR &&
                ty_is_integer(ty->pointee) &&
                ty_size(ty->pointee) == (init->str_width ? init->str_width : 1)) {
                int len = (int)init->num, esz = ty_size(ty->pointee);
                if (ty->count && ty->count < len - 1)
                    sema_error_at(u, init->line, init->col,
                               "initializer is longer than the array");
                for (int i = 0; i < len && (!ty->count || i < ty->count);
                     i++) {
                    struct expr *ch = xcalloc(1, sizeof *ch);
                    ch->kind = EXPR_NUM;
                    ch->line = init->line;
                    /* element i: esz little-endian bytes (lit_encode) */
                    unsigned long uv = 0;
                    for (int b = 0; b < esz; b++)
                        uv |= (unsigned long)(unsigned char)
                              init->name[(size_t)i * (size_t)esz + (size_t)b]
                              << (8 * b);
                    ch->num = (long)uv;
                    ch->ty = ty->pointee;
                    init_push(out, off + i * esz, ty->pointee, ch);
                }
                return;
            }
            sema_error_at(u, init->line, init->col,
                       "an array needs a brace initializer or a string");
        }
        check_expr(u, f, sc, init);
        if (ty->kind != TY_STRUCT)
            if (!ty_is_complex(init->ty))
            need_scalar(u, init, "an initializer");
        init_push(out, off, ty,
                  convert_assign(u, init, ty, "initialization"));
        return;
    }

    if (ty->kind == TY_ARRAY) {
        int esz = ty_size(ty->pointee);
        /* Positional by default; a `[i] =` designator repositions the
         * running index and initialization continues positionally after it
         * (later writes to the same slot win, matching C). */
        int ai = 0;
        for (int i = 0; i < init->nelems; i++) {
            struct expr *el = init->elems[i];
            if (el->desig_field)
                sema_error_at(u, el->line, el->col,
                           "field designator '.%s' in an array initializer",
                           el->desig_field);
            if (el->desig_index >= 0)
                ai = el->desig_index;
            /* GNU range `[lo ... hi] = v`: place v at every index in the
             * span. A zero value needs no leaves — the object is already
             * zero-filled (static bytes start zero; a local is memzeroed) —
             * which also keeps a huge `[a ... b] = 0` cheap. */
            int hi = el->desig_index_hi >= 0 ? el->desig_index_hi : ai;
            if (ty->count && hi >= ty->count)
                sema_error_at(u, el->line, el->col,
                           "initializer index %d is past the end of an "
                           "array of %d", hi, ty->count);
            int is_zero = el->kind == EXPR_NUM && el->num == 0;
            for (; ai <= hi; ai++)
                if (!(el->desig_index_hi >= 0 && is_zero))
                    flatten_init(u, f, sc, el, ty->pointee,
                                 off + ai * esz, out);
        }
        return;
    }
    if (ty->kind == TY_STRUCT) {
        /* Positional by default; a `.field =` designator jumps to that
         * member and initialization continues positionally after it. */
        int mi = 0;
        for (int i = 0; i < init->nelems; i++) {
            struct expr *el = init->elems[i];
            if (el->desig_field) {
                struct member *m = ty_find_member(ty, el->desig_field);
                if (!m)
                    sema_error_at(u, el->line, el->col,
                               "%s has no member '%s'", ty_name(ty),
                               el->desig_field);
                mi = (int)(m - ty->members);
            }
            /* An unnamed bitfield (padding, or a `:0` separator) takes no
             * initializer — skip past it in positional order. */
            while (mi < ty->nmembers && ty->members[mi].is_bitfield &&
                   !ty->members[mi].name)
                mi++;
            if (mi >= ty->nmembers)
                sema_error_at(u, init->line, init->col,
                           "too many initializers for %s, which has %d "
                           "members", ty_name(ty), ty->nmembers);
            struct member *m = &ty->members[mi];
            if (m->is_bitfield) {
                /* A bitfield leaf: its value is masked and merged into the
                 * shared storage unit at m->off by both lowerings, so it
                 * carries (bit_off, bit_width) rather than a byte width. */
                check_expr(u, f, sc, el);
                need_scalar(u, el, "a bitfield initializer");
                struct expr *cv = convert_assign(u, el, m->ty,
                                                 "initialization");
                init_push_bf(out, off + m->off, m->ty, cv,
                             m->bit_off, m->bit_width, m->bf_bytes);
                mi++;
                continue;
            }
            flatten_init(u, f, sc, el, m->ty, off + m->off, out);
            mi++;
        }
        return;
    }
    /* a braced scalar: { x } */
    if (init->nelems != 1)
        sema_error_at(u, init->line, init->col,
                   "a scalar takes exactly one initializer");
    flatten_init(u, f, sc, init->elems[0], ty, off, out);
}

/* Resolve a constant-address expression (the value of a pointer slot in a
 * static initializer) to a target global/function plus a byte addend:
 * `&g`, a decayed array/function, `&arr[i]`, `&g.field`, `p + n`. Returns 1
 * on success, filling *gt or *ft and adding to *add. */
static int resolve_addr(struct expr *e, struct global **gt,
                        struct func **ft, long *add)
{
    while (e && e->kind == EXPR_CAST)
        e = e->rhs;
    if (!e)
        return 0;
    if (e->kind == EXPR_VAR && e->gref) { *gt = e->gref; return 1; }
    if (e->kind == EXPR_VAR && e->fref) { *ft = e->fref; return 1; }
    if (e->kind == EXPR_BINOP && (e->op == B_ADD || e->op == B_SUB)) {
        int esz = e->ty && e->ty->kind == TY_PTR
                ? ty_size(e->ty->pointee) : 1;
        long k;
        if (resolve_addr(e->lhs, gt, ft, add) && const_fold(e->rhs, &k)) {
            *add += (e->op == B_SUB ? -k : k) * esz;
            return 1;
        }
        if (e->op == B_ADD && const_fold(e->lhs, &k) &&
            resolve_addr(e->rhs, gt, ft, add)) {
            *add += k * esz;
            return 1;
        }
        return 0;
    }
    if (e->kind == EXPR_ADDR) {
        struct expr *lv = e->rhs;
        while (lv && lv->kind == EXPR_CAST)
            lv = lv->rhs;
        if (!lv)
            return 0;
        if (lv->kind == EXPR_DEREF)            /* &*x == x */
            return resolve_addr(lv->rhs, gt, ft, add);
        if (lv->kind == EXPR_VAR && lv->gref) { *gt = lv->gref; return 1; }
        if (lv->kind == EXPR_MEMBER && lv->memb) {
            struct expr *base = lv->is_arrow ? lv->lhs : NULL;
            if (lv->is_arrow) {                /* &p->f : p + off */
                if (!resolve_addr(base, gt, ft, add))
                    return 0;
            } else {                           /* &b.f : &b + off */
                struct expr addr = { 0 };
                addr.kind = EXPR_ADDR;
                addr.rhs = lv->lhs;
                if (!resolve_addr(&addr, gt, ft, add))
                    return 0;
            }
            *add += lv->memb->off;
            return 1;
        }
    }
    return 0;
}

/* Lower flattened initializer leaves into a static object's byte image
 * plus a relocation list. Shared by file-scope globals and static locals
 * — both have static storage, so every leaf must reduce to constant
 * bytes now, save pointer slots initialized by a string literal, which
 * become relocations the linker resolves. */
static void lower_static_bytes(struct unit *u, int line, int size,
                               struct initelem *v, int n,
                               const char **out_bytes,
                               struct greloc **out_rel, int *out_nrel)
{
    char *bytes = xcalloc(1, (size_t)(size ? size : 1));
    struct greloc *rel = NULL;
    int nrel = 0, caprel = 0;
    for (int k = 0; k < n; k++) {
        struct expr *core = v[k].e;
        while (core && core->kind == EXPR_CAST)
            core = core->rhs;
        /* The address of a global: `&g`, or an array/function global that
         * decayed to a pointer (`char **environ = embk_empty_env`). */
        struct global *gt = NULL;
        struct func *ft = NULL;
        long addend = 0;
        if (core && core->kind != EXPR_STR)
            resolve_addr(core, &gt, &ft, &addend);
        if ((core && core->kind == EXPR_STR && v[k].ty->kind == TY_PTR) ||
            ((gt || ft) && v[k].ty->kind == TY_PTR)) {
            /* a pointer slot: zero bytes stay, the linker writes the address
             * of a string literal, a global (+addend), or a function. */
            if (nrel == caprel) {
                caprel = caprel ? caprel * 2 : 4;
                rel = xrealloc(rel, (size_t)caprel * sizeof *rel);
            }
            rel[nrel].off = v[k].off;
            rel[nrel].str = (gt || ft) ? NULL : core->name;
            rel[nrel].str_len = (gt || ft) ? 0 : (int)core->num;
            rel[nrel].str_width = (gt || ft) ? 1
                                  : (core->str_width ? core->str_width : 1);
            rel[nrel].gtarget = gt;
            rel[nrel].ftarget = ft;
            rel[nrel].addend = addend;
            nrel++;
            continue;
        }
        int sz = ty_size(v[k].ty);
        /* A complex slot: both parts folded in its element's format */
        if (ty_is_complex(v[k].ty)) {
            struct type *el = v[k].ty->celem;
            enum ldf_fmt fmt = el->kind == TY_FLOAT ? LDF_FLOAT
                             : el->kind == TY_DOUBLE ? LDF_DOUBLE
                             : ldf_target_fmt();
            struct ldf *re, *im;
            if (!cx_fold(v[k].e, fmt, &re, &im))
                sema_error_line(u, line,
                           "a static complex initializer must be a constant "
                           "that EmbCC folds: literals, casts, + and -, and "
                           "scaling by a real");
            int esz = ty_size(el);
            unsigned char pb[16];
            ldf_encode(re, fmt, pb);
            memcpy(bytes + v[k].off, pb, (size_t)esz);
            ldf_encode(im, fmt, pb);
            memcpy(bytes + v[k].off + esz, pb, (size_t)esz);
            continue;
        }
        /* A long double slot: its exact value in the target's format */
        if (v[k].ty->kind == TY_LDOUBLE) {
            struct ldf *x = const_fold_ld(v[k].e);
            if (!x)
                sema_error_line(u, line,
                           "a static long double initializer must be a "
                           "constant expression");
            unsigned char lb[16];
            ldf_encode(x, ldf_target_fmt(), lb);
            memcpy(bytes + v[k].off, lb, 16);
            continue;
        }
        /* A float/double slot: fold to the value, store its IEEE-754 bit
         * pattern (4 bytes for float, 8 for double), little-endian. */
        if (ty_is_float(v[k].ty)) {
            double dv;
            if (!const_fold_f(v[k].e, &dv))
                sema_error_line(u, line,
                           "a static float initializer must be a constant "
                           "expression");
            unsigned long ubits;
            if (v[k].ty->kind == TY_FLOAT) {
                float fv = (float)dv; unsigned int u32;
                memcpy(&u32, &fv, 4); ubits = u32;
            } else {
                memcpy(&ubits, &dv, 8);
            }
            for (int b = 0; b < sz; b++)
                bytes[v[k].off + b] = (char)(ubits >> (8 * b));
            continue;
        }
        if (v[k].ty->kind == TY_INT128) {
            struct w128 w;
            if (!const_fold128(v[k].e, &w))
                sema_error_line(u, line,
                           "a static __int128 initializer must be a constant "
                           "expression");
            if (v[k].bit_width) {
                merge_bits(bytes + v[k].off,
                           v[k].bf_bytes ? v[k].bf_bytes : 16, w,
                           v[k].bit_off, v[k].bit_width);
                continue;
            }
            for (int b = 0; b < 8; b++) {
                bytes[v[k].off + b] = (char)(w.lo >> (8 * b));
                bytes[v[k].off + 8 + b] = (char)(w.hi >> (8 * b));
            }
            continue;
        }
        long cv;
        if (!const_fold(v[k].e, &cv))
            sema_error_line(u, line,
                       "a static initializer must be a constant, a "
                       "string literal, or the address of a global");
        if (v[k].bit_width) {
            merge_bits(bytes + v[k].off, v[k].bf_bytes ? v[k].bf_bytes : sz,
                       w_make((unsigned long)cv, 0), v[k].bit_off,
                       v[k].bit_width);
            continue;
        }
        for (int b = 0; b < sz; b++)
            bytes[v[k].off + b] = (char)((unsigned long)cv >> (8 * b));
    }
    *out_bytes = bytes;
    *out_rel = rel;
    *out_nrel = nrel;
}

/* Reduce every file-scope global's aggregate/relocatable initializer to
 * its byte image + relocations, now that types and sizes are settled. */
static void lower_globals(struct unit *u)
{
    struct func gf = { 0 };
    gf.name = "<global initializer>";
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined || !g->init_expr)
            continue;
        /* An initializer may reference any name declared before this
         * global — mirror the source position so the declare-before-use
         * rule (and enum-constant visibility) matches C. */
        cur_body_seq = g->def_seq;
        struct scope sc = { 0, 0, 0, 0 };
        struct initbuf ib = { 0, 0, 0 };
        g_in_static_init++;
        flatten_init(u, &gf, &sc, g->init_expr, g->ty, 0, &ib);
        g_in_static_init--;
        lower_static_bytes(u, g->line, ty_size(g->ty), ib.v, ib.n,
                           &g->init_bytes, &g->relocs, &g->nrelocs);
        g->init_len = ty_size(g->ty);
        g->init_expr = NULL;
    }
}

/* Register name -> encoding number (0-15). The table type is file scope
 * because EmbCC's own subset (which compiles this file) has no block-scope
 * type definitions. */
struct regname { const char *name; int reg; };
static const struct regname reg_names[] = {
    { "rax", 0 }, { "rbx", 3 }, { "rcx", 1 }, { "rdx", 2 },
    { "rsi", 6 }, { "rdi", 7 }, { "r8", 8 }, { "r9", 9 },
    { "r10", 10 }, { "r11", 11 }, { "r12", 12 }, { "r13", 13 },
    { "r14", 14 }, { "r15", 15 },
};
static int asm_reg_by_name(const char *n)
{
    for (unsigned i = 0; i < sizeof reg_names / sizeof reg_names[0]; i++)
        if (strcmp(n, reg_names[i].name) == 0)
            return reg_names[i].reg;
    return -1;
}

int builtin_bitop(const char *bn, int *width)
{
    static const char *const ops[] = { "ctz", "clz", "popcount", "ffs",
                                       "parity", "clrsb" };
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        size_t n = strlen(ops[i]);
        if (strncmp(bn, ops[i], n) != 0)
            continue;
        const char *sfx = bn + n;
        int w = !*sfx ? 4 : (strcmp(sfx, "l") == 0 || strcmp(sfx, "ll") == 0) ? 8 : 0;
        if (!w)
            continue;
        if (width)
            *width = w;
        return (int)i + 1;
    }
    return 0;
}

enum atomic_kind atomic_builtin(const char *name, int *op)
{
    static const struct { const char *name; enum atomic_kind kind; } fixed[] = {
        { "__atomic_load_n",             AK_LOAD_N },
        { "__atomic_store_n",            AK_STORE_N },
        { "__atomic_exchange_n",         AK_EXCHANGE_N },
        { "__atomic_compare_exchange_n", AK_CMPXCHG_N },
        { "__atomic_load",               AK_LOAD },
        { "__atomic_store",              AK_STORE },
        { "__atomic_exchange",           AK_EXCHANGE },
        { "__atomic_compare_exchange",   AK_CMPXCHG },
        { "__atomic_test_and_set",       AK_TEST_AND_SET },
        { "__atomic_clear",              AK_CLEAR },
        { "__atomic_thread_fence",       AK_THREAD_FENCE },
        { "__atomic_signal_fence",       AK_SIGNAL_FENCE },
        { "__atomic_always_lock_free",   AK_LOCK_FREE },
        { "__atomic_is_lock_free",       AK_LOCK_FREE },
        { "__sync_bool_compare_and_swap", AK_SYNC_BOOL_CAS },
        { "__sync_val_compare_and_swap",  AK_SYNC_VAL_CAS },
        { "__sync_lock_test_and_set",     AK_SYNC_LOCK_TAS },
        { "__sync_lock_release",          AK_SYNC_LOCK_RELEASE },
    };
    static const struct { const char *name; int op; } ops[] = {
        { "add", '+' }, { "sub", '-' }, { "and", '&' },
        { "or", '|' },  { "xor", '^' }, { "nand", 'n' },
    };
    *op = 0;
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        if (strcmp(name, fixed[i].name) == 0)
            return fixed[i].kind;
    /* __atomic_fetch_OP / __atomic_OP_fetch / __sync_fetch_and_OP /
     * __sync_OP_and_fetch */
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        char buf[48];
        *op = ops[i].op;
        snprintf(buf, sizeof buf, "__atomic_fetch_%s", ops[i].name);
        if (strcmp(name, buf) == 0) return AK_FETCH_OP;
        snprintf(buf, sizeof buf, "__atomic_%s_fetch", ops[i].name);
        if (strcmp(name, buf) == 0) return AK_OP_FETCH;
        snprintf(buf, sizeof buf, "__sync_fetch_and_%s", ops[i].name);
        if (strcmp(name, buf) == 0) return AK_FETCH_OP;
        snprintf(buf, sizeof buf, "__sync_%s_and_fetch", ops[i].name);
        if (strcmp(name, buf) == 0) return AK_OP_FETCH;
    }
    *op = 0;
    return AK_NONE;
}

/* The object an atomic builtin works on must be one the machines can move
 * in one access: an integer or a pointer of 1, 2, 4 or 8 bytes — or an
 * __int128, which both do with a 16-byte compare-and-swap (irgen). */
static void need_atomic_object(struct unit *u, struct expr *e, struct type *t)
{
    int sz = ty_size(t);
    if (!(ty_is_integer(t) || t->kind == TY_PTR) ||
        (sz != 1 && sz != 2 && sz != 4 && sz != 8 && t->kind != TY_INT128))
        sema_error_at(u, e->line, e->col,
                "%s works on an integer or pointer of 1, 2, 4, 8 or 16 "
                "bytes, not %s", e->lhs->name, ty_name(t));
}

/* Types a call to an atomic builtin (see atomic_builtin). */
static void check_atomic_call(struct unit *u, struct func *f,
                              struct scope *sc, struct expr *e,
                              enum atomic_kind ak)
{
    static const signed char nargs[] = {
        [AK_LOAD_N] = 2, [AK_STORE_N] = 3, [AK_EXCHANGE_N] = 3,
        [AK_CMPXCHG_N] = 6, [AK_LOAD] = 3, [AK_STORE] = 3,
        [AK_EXCHANGE] = 4, [AK_CMPXCHG] = 6, [AK_FETCH_OP] = 3,
        [AK_OP_FETCH] = 3, [AK_TEST_AND_SET] = 2, [AK_CLEAR] = 2,
        [AK_THREAD_FENCE] = 1, [AK_SIGNAL_FENCE] = 1, [AK_LOCK_FREE] = 2,
        [AK_SYNC_BOOL_CAS] = 3, [AK_SYNC_VAL_CAS] = 3, [AK_SYNC_LOCK_TAS] = 2,
        [AK_SYNC_LOCK_RELEASE] = 1,
    };
    const char *name = e->lhs->name;
    for (int i = 0; i < e->nargs; i++)
        check_expr(u, f, sc, e->args[i]);
    /* The __sync forms are variadic in gcc (a trailing list of variables to
     * protect, ignored); the __atomic ones fix their count. The fetch/op
     * forms share one entry, so the family decides. */
    int want = nargs[ak];
    if (ak == AK_FETCH_OP || ak == AK_OP_FETCH)
        want = strncmp(name, "__sync_", 7) == 0 ? 2 : 3;
    int is_sync = strncmp(name, "__sync_", 7) == 0;
    if (e->nargs < want || (!is_sync && e->nargs != want))
        sema_error_at(u, e->line, e->col, "%s takes %d arguments, not %d",
                name, want, e->nargs);
    e->name = name;

    if (ak == AK_THREAD_FENCE || ak == AK_SIGNAL_FENCE) {
        e->ty = ty_base(TY_VOID, 0);
        return;
    }
    if (ak == AK_LOCK_FREE) {
        /* Every 1/2/4/8-byte object is lock-free on both targets; fold to a
         * constant so `_Static_assert(__atomic_always_lock_free(...))` works. */
        long n;
        if (!const_fold(e->args[0], &n))
            sema_error_at(u, e->line, e->col,
                    "%s needs a constant size", name);
        struct expr *c = e;
        int line = c->line, col = c->col;
        memset(c, 0, sizeof *c);
        c->kind = EXPR_NUM;
        c->line = line;
        c->col = col;
        c->num = (n == 1 || n == 2 || n == 4 || n == 8);
        c->ty = ty_base(TY_BOOL, 0);
        return;
    }

    if (e->args[0]->ty->kind != TY_PTR)
        sema_error_at(u, e->line, e->col,
                "%s needs a pointer first argument", name);
    struct type *obj = e->args[0]->ty->pointee;

    /* test_and_set / clear work on one byte, whatever the pointer says. */
    if (ak == AK_TEST_AND_SET) { e->ty = ty_base(TY_BOOL, 0); return; }
    if (ak == AK_CLEAR)        { e->ty = ty_base(TY_VOID, 0); return; }

    need_atomic_object(u, e, obj);
    /* The generic forms pass values by pointer; each must point at an
     * object the same size as the atomic one. */
    if (ak == AK_LOAD || ak == AK_STORE || ak == AK_EXCHANGE ||
        ak == AK_CMPXCHG || ak == AK_CMPXCHG_N) {
        int last = ak == AK_EXCHANGE ? 2 : ak == AK_LOAD || ak == AK_STORE ? 1 : 2;
        for (int k = 1; k <= last; k++) {
            if (ak == AK_CMPXCHG_N && k == 2)
                break;                           /* desired is a value */
            struct type *pt = e->args[k]->ty;
            if (pt->kind != TY_PTR || ty_size(pt->pointee) != ty_size(obj))
                sema_error_at(u, e->line, e->col,
                        "argument %d of %s must point to a %d-byte object",
                        k + 1, name, ty_size(obj));
        }
    }
    switch (ak) {
    case AK_STORE_N: case AK_LOAD: case AK_STORE: case AK_EXCHANGE:
    case AK_SYNC_LOCK_RELEASE:
        e->ty = ty_base(TY_VOID, 0);
        break;
    case AK_CMPXCHG_N: case AK_CMPXCHG: case AK_SYNC_BOOL_CAS:
        e->ty = ty_base(TY_BOOL, 0);
        break;
    default:
        e->ty = obj;                              /* the value itself */
        break;
    }
}

/* Map a fixed-register constraint letter to its register (0-15), else -1. */
static int asm_fixed_letter(char c)
{
    switch (c) {
    case 'a': return 0;
    case 'b': return 3;
    case 'c': return 1;
    case 'd': return 2;
    case 'S': return 6;
    case 'D': return 7;
    }
    return -1;
}

/* The register a constraint pins its operand to: a fixed one for a/b/c/d/S/D,
 * the bound register of a `register T x __asm__("r10")` variable, or the
 * sentinel -2 for an allocatable class (r/q/g/m/R) that irgen assigns from a
 * free register. Output constraints carry a leading '=' or '+'; '&' is
 * accepted and ignored. */
/* aarch64 operand resolution. The letters mean different things there, and
 * x86's fixed-register letters (a/b/c/d/S/D) mean nothing at all, so this is
 * a separate function rather than a branch through the x86 one:
 *   - a `register T v __asm__("x0")` variable binds its register;
 *   - 'i'/'n' (without 'r') on a constant folds to a literal immediate;
 *   - 'r' / 'g' is allocatable (-2), assigned in irgen;
 *   - anything else is ASM_REG_INVALID, refused by irgen if emitted. */
static int asm_resolve_reg_arm64(struct unit *u, struct stmt *s,
                                 struct asm_operand *op, const char *c)
{
    if (op->expr->kind == EXPR_VAR && op->expr->asm_reg) {
        const char *rn = op->expr->asm_reg;
        int r = a64asm_gpr(rn, (int)strlen(rn));
        /* x12 is the codegen's address scratch and x16..x18 are the
         * intra-procedure-call and platform registers; x19 and up are
         * callee-saved, which a register variable would have to preserve
         * and EmbCC does not yet save around asm. */
        if (r < 0 || r == 12 || r >= 16)
            sema_error_at(u, s->line, s->col,
                    "register variable bound to '%s' is not supported for "
                    "aarch64 asm (use x0..x11 or x13..x15)", rn);
        return r;
    }
    int has_r = 0, has_i = 0;
    for (const char *p = c; *p; p++) {
        if (*p == 'r' || *p == 'g') has_r = 1;
        if (*p == 'i' || *p == 'n') has_i = 1;
    }
    if (has_i && !has_r) {
        long v;
        if (const_fold(op->expr, &v)) {
            op->is_imm = 1;
            op->imm = v;
            return ASM_REG_IMM;
        }
        return ASM_REG_INVALID;     /* a non-constant "i": gcc refuses too */
    }
    if (has_r)
        return -2;
    return ASM_REG_INVALID;
}

static int asm_resolve_reg(struct unit *u, struct stmt *s,
                           struct asm_operand *op, int is_out)
{
    const char *c = op->constraint;
    if (is_out && *c != '=' && *c != '+')
        sema_error_at(u, s->line, s->col,
                   "an asm output constraint must start with '=' or '+' "
                   "(got \"%s\")", op->constraint);
    while (*c == '=' || *c == '+' || *c == '&')
        c++;
    if (target_get() == TARGET_AARCH64)
        return asm_resolve_reg_arm64(u, s, op, c);
    for (const char *p = c; *p; p++) {           /* a fixed register wins */
        int r = asm_fixed_letter(*p);
        if (r >= 0)
            return r;
    }
    if (op->expr->kind == EXPR_VAR && op->expr->asm_reg) {
        int r = asm_reg_by_name(op->expr->asm_reg);
        if (r >= 0)
            return r;
    }
    for (const char *p = c; *p; p++)             /* else allocate a register */
        if (*p == 'r' || *p == 'q' || *p == 'g' || *p == 'm' || *p == 'R' ||
            *p == 'i' || *p == 'n')
            /* an immediate ('i'/'n'): EmbCC has no way to substitute a literal
             * (a symbol address needs a relocation), so it computes the value
             * into a register and the template's mov uses that register — the
             * result is identical, only the encoding differs from gcc's. */
            return -2;
    for (const char *p = c; *p; p++)             /* an SSE/XMM ('x') operand */
        if (*p == 'x')
            return -3;                            /* irgen allocates an xmm */
    sema_error_at(u, s->line, s->col,
               "asm constraint \"%s\" is not supported "
               "(EmbCC handles a/b/c/d/S/D, 'r'/'q'/'g'/'m', 'x', and a "
               "register-asm variable)", op->constraint);
    return -1;
}

/* Declarations anywhere in the function share one flat scope, and
 * shadowing is rejected outright. C gives inner blocks their own scope;
 * refusing shadowed names accepts strictly fewer programs than C does,
 * so the subset stays a subset. */
static void check_stmt(struct unit *u, struct func *f, struct scope *sc,
                       struct stmt *s_in, int in_loop, int in_switch,
                       int at_sw_level)
{
    /* Each statement is a recovery point: an error in one is reported and
     * the next is still checked. What a failed statement already added to
     * the scope stays — dropping it would turn one error into several
     * ("not declared") further down. `s` is written between setjmp and
     * longjmp, so it is volatile (C99 7.13.2.1p3). */
    struct stmt *volatile s = s_in;
    jmp_buf jb, *save = g_recover;
    for (; s; s = s->next) {
        g_recover = &jb;
        if (setjmp(jb))
            continue;
        switch (s->kind) {
        case STMT_BREAK:
            /* break leaves the nearest loop OR switch; continue only
             * ever belongs to a loop. */
            if (!in_loop && !in_switch)
                sema_error_at(u, s->line, s->col,
                           "'break' outside of a loop or switch");
            break;
        case STMT_CONTINUE:
            if (!in_loop)
                sema_error_at(u, s->line, s->col, "'continue' outside of a loop");
            break;
        case STMT_CASE:
        case STMT_DEFAULT:
            if (!at_sw_level)
                sema_error_at(u, s->line, s->col,
                           "'%s' must appear directly in its switch body "
                           "(labels inside a nested block are not "
                           "supported)",
                           s->kind == STMT_CASE ? "case" : "default");
            if (s->kind == STMT_CASE) {
                check_expr(u, f, sc, s->expr);
                need_integer(u, s->expr, "a case label");
                if (!const_fold(s->expr, &s->cval))
                    sema_error_at(u, s->line, s->col,
                               "a case label must be an integer constant "
                               "expression");
            }
            break;
        case STMT_SWITCH: {
            check_expr(u, f, sc, s->cond);
            need_integer(u, s->cond, "'switch'");
            struct stmt *list = switch_stmts(s->body);
            check_stmt(u, f, sc, list, in_loop, 1, 1);
            /* duplicate labels and a second default are parse-time
             * errors, not a runtime coin flip about which one wins */
            int ndefault = 0;
            for (struct stmt *a = list; a; a = a->next) {
                if (a->kind == STMT_DEFAULT && ++ndefault > 1)
                    sema_error_line(u, a->line,
                               "a switch can have only one 'default'");
                if (a->kind != STMT_CASE)
                    continue;
                for (struct stmt *b = a->next; b; b = b->next)
                    if (b->kind == STMT_CASE && b->cval == a->cval)
                        sema_error_line(u, b->line,
                                   "duplicate case label %ld", b->cval);
            }
            break;
        }
        case STMT_DO:
            check_stmt(u, f, sc, s->body, 1, in_switch, 0);
            check_expr(u, f, sc, s->cond);
            if (ty_is_complex(s->cond->ty) && cx_lowering())
                s->cond = cx_truth(s->cond);
            else
                need_scalar(u, s->cond, "'do'/'while'");
            break;
        case STMT_DECL:
            if (s->is_extern) {
                /* block-scope extern: no storage here, external linkage. Register
                 * the unit global/function (safe now -- parsing is done, so the
                 * list tails are no longer live) if not already present, and for
                 * a variable wire a block-scope entry onto it so references
                 * resolve. A function needs no var entry (calls use find_func). */
                if (s->dty->kind == TY_FUNC) {
                    if (!find_func(u, s->name)) {
                        struct func *g = xcalloc(1, sizeof *g);
                        g->name = s->name; g->file = u->file; g->line = s->line;
                        g->seq = f->seq; g->declared = 1;
                        g->ret_ty = s->dty->ret;
                        g->nparams = s->dty->nptypes;
                        for (int k = 0; k < s->dty->nptypes; k++)
                            g->param_tys[k] = s->dty->ptypes[k];
                        g->is_varargs = s->dty->is_varargs;
                        struct func **ft = &u->funcs;
                        while (*ft) ft = &(*ft)->next;
                        *ft = g;
                    }
                } else {
                    struct global *g = find_global(u, s->name);
                    if (!g) {
                        g = xcalloc(1, sizeof *g);
                        g->name = s->name; g->ty = s->dty; g->is_extern = 1;
                        g->file = u->file; g->line = s->line;
                        g->seq = f->seq; g->def_seq = f->seq;
                        struct global **gt = &u->globals;
                        while (*gt) gt = &(*gt)->next;
                        *gt = g;
                    }
                    s->var_index = scope_add(sc, s->name, s->dty, NULL);
                    sc->vars[s->var_index].line = s->line;
                    sc->vars[s->var_index].col = s->col;
                    sc->vars[s->var_index].g = g;
                    s->sglob = g;   /* irgen: this decl carries no local storage */
                }
                break;
            }
            if (ty_is_vm(s->dty)) {
                if (s->is_static)
                    sema_error_at(u, s->line, s->col,
                            "static '%s' cannot have a variably modified "
                            "type (%s)", s->name, ty_name(s->dty));
                if (ty_is_vla(s->dty) && s->expr)
                    sema_error_at(u, s->line, s->col,
                            "variable length array '%s' cannot be "
                            "initialized", s->name);
                /* sizes first: the lengths are evaluated where the
                 * declaration is reached, before the name is in scope */
                vla_prepare(u, f, sc, s->dty);
                if (ty_is_vla(s->dty))
                    s->vla_sp = scope_add(sc, "<vla sp>",
                                          ty_base(TY_LONG, 1), NULL);
            }
            if (s->expr && s->dty->kind == TY_ARRAY &&
                s->expr->kind == EXPR_STR) {
                /* char a[] = "..." (or a wide array from L""/u""/U"") : the
                 * element must be an integer matching the literal's width; an
                 * omitted size is the literal's element count. */
                int w = s->expr->str_width ? s->expr->str_width : 1;
                if (!ty_is_integer(s->dty->pointee) ||
                    ty_size(s->dty->pointee) != w)
                    sema_error_at(u, s->line, s->col,
                               "a string literal can only initialize an integer "
                               "array whose element width matches it");
                if (s->dty->count == 0)
                    s->dty = ty_array(s->dty->pointee,
                                      (int)s->expr->num);
            } else if (s->expr && s->expr->kind == EXPR_INITLIST &&
                       s->dty->kind == TY_ARRAY && s->dty->count == 0) {
                /* an omitted array size is the highest index reached */
                s->dty = ty_array(s->dty->pointee,
                                  initlist_array_count(s->expr));
            }
            /* The name is in scope WITHIN its own initializer (C11
             * 6.2.1p7: scope begins just after the declarator), so the
             * pervasive `T *p = xcalloc(1, sizeof *p)` resolves p. Add
             * it before checking the initializer; a static local's
             * global is wired onto this same entry below. */
            if (scope_find_here(sc, s->name) >= 0)
                sema_error_at(u, s->line, s->col,
                           "'%s' is already declared in this block",
                           s->name);
            warn_shadow(u, sc, s->name, s->line, s->col);
            s->var_index = scope_add(sc, s->name, s->dty, NULL);
            sc->vars[s->var_index].line = s->line;
            sc->vars[s->var_index].col = s->col;
            sc->vars[s->var_index].asm_reg = s->asm_reg;
            sc->vars[s->var_index].user_align = s->user_align;
            /* A static local has static storage, so a compound literal in its
             * initializer is an anonymous global, not a stack slot. */
            if (s->is_static)
                g_in_static_init++;
            if (s->expr && s->dty->kind != TY_ARRAY &&
                s->dty->kind != TY_STRUCT) {
                check_expr(u, f, sc, s->expr);
                if (s->dty->kind != TY_STRUCT)
                    if (!ty_is_complex(s->expr->ty))
                    need_scalar(u, s->expr, "an initializer");
                s->expr = convert_assign(u, s->expr, s->dty,
                                         "initialization");
            }
            if (s->expr && (s->expr->kind == EXPR_INITLIST ||
                            s->dty->kind == TY_ARRAY ||
                            s->dty->kind == TY_STRUCT)) {
                struct initbuf ib = { 0, 0, 0 };
                flatten_init(u, f, sc, s->expr, s->dty, 0, &ib);
                s->inits = ib.v;
                s->ninits = ib.n;
                s->expr = NULL;
            }
            if (s->is_static)
                g_in_static_init--;
            if (s->is_static) {
                /* A static local has static STORAGE and internal
                 * linkage: it becomes a global of its own, named so it
                 * cannot collide with a file-scope name, and its
                 * initializer is lowered to a byte image + relocations
                 * through the same path as any file-scope global. */
                struct global *g = xcalloc(1, sizeof *g);
                size_t n = strlen(f->name) + strlen(s->name) + 8;
                char *nm = xmalloc(n);
                snprintf(nm, n, "%s.%s", f->name, s->name);
                g->name = nm;
                g->line = s->line;
                g->seq = -1;      /* visible from its own function only */
                g->ty = s->dty;
                g->is_static = 1;
                g->defined = 1;
                g->used = 1;
                /* Aggregates arrive pre-flattened in s->inits; a scalar's
                 * value is a single leaf at offset 0. Every field is set:
                 * lower_static_bytes treats a nonzero bit_width as a
                 * BITFIELD, so a stale one on the stack wrote a plain
                 * scalar's value as garbage bits at a garbage offset —
                 * kernel/net/udp/udp.c's `static uint16_t eph = 49152`
                 * came out 0, and only whatever the stack held decided. */
                struct initelem one = { 0, NULL, NULL, 0, 0, 0 };
                struct initelem *iv = s->inits;
                int in = s->ninits;
                if (!in && s->expr) {
                    one.off = 0;
                    one.ty = s->dty;
                    one.e = s->expr;
                    iv = &one;
                    in = 1;
                }
                if (in) {
                    lower_static_bytes(u, s->line, ty_size(s->dty), iv, in,
                                       &g->init_bytes, &g->relocs,
                                       &g->nrelocs);
                    g->init_len = ty_size(s->dty);
                    g->has_init = 1;
                }
                s->ninits = 0;
                struct global **gt = &u->globals;
                while (*gt)
                    gt = &(*gt)->next;
                *gt = g;
                s->sglob = g;
                s->expr = NULL;   /* the data, not code, carries it */
                sc->vars[s->var_index].g = g; /* wire the global onto the
                                                 entry added above */
                break;
            }
            break;
        case STMT_RETURN:
            if (f->ret_ty->kind == TY_VOID) {
                if (s->expr)
                    sema_error_at(u, s->line, s->col,
                               "returning a value from void '%s'",
                               f->name);
            } else {
                if (!s->expr)
                    sema_error_at(u, s->line, s->col,
                               "'%s' returns %s; 'return' needs a value",
                               f->name, ty_name(f->ret_ty));
                check_expr(u, f, sc, s->expr);
                if (s->expr->ty->kind != TY_STRUCT)
                    if (!ty_is_complex(s->expr->ty))
                        need_scalar(u, s->expr, "'return'");
                s->expr = convert_assign(u, s->expr, f->ret_ty, "return");
            }
            break;
        case STMT_EXPR:
            check_expr(u, f, sc, s->expr);
            break;
        case STMT_ASM: {
            struct asm_stmt *a = s->asm_s;
            for (int i = 0; i < a->nout; i++) {
                check_expr(u, f, sc, a->out[i].expr);
                if (!is_lvalue(a->out[i].expr))
                    sema_error_at(u, s->line, s->col,
                               "an asm output operand must be an lvalue");
                a->out[i].reg = asm_resolve_reg(u, s, &a->out[i], 1);
            }
            for (int i = 0; i < a->nin; i++) {
                check_expr(u, f, sc, a->in[i].expr);
                a->in[i].reg = asm_resolve_reg(u, s, &a->in[i], 0);
            }
            /* the template is assembled in irgen, once -2 (allocatable)
             * operands have been assigned registers */
            break;
        }
        case STMT_IF:
            check_expr(u, f, sc, s->cond);
            if (ty_is_complex(s->cond->ty) && cx_lowering())
                s->cond = cx_truth(s->cond);
            else
                need_scalar(u, s->cond, "'if'");
            check_stmt(u, f, sc, s->thn, in_loop, in_switch, 0);
            if (s->els)
                check_stmt(u, f, sc, s->els, in_loop, in_switch, 0);
            break;
        case STMT_WHILE:
            check_expr(u, f, sc, s->cond);
            if (ty_is_complex(s->cond->ty) && cx_lowering())
                s->cond = cx_truth(s->cond);
            else
                need_scalar(u, s->cond, "'while'");
            check_stmt(u, f, sc, s->body, 1, in_switch, 0);
            break;
        case STMT_FOR: {
            /* `for (int i = ...)` scopes i to the loop, so sibling
             * loops may each declare their own. */
            int mark = sc->n, prev = sc->block_start;
            sc->block_start = mark;
            if (s->initdecl)
                check_stmt(u, f, sc, s->initdecl, in_loop, in_switch, 0);
            if (s->init)
                check_expr(u, f, sc, s->init);
            if (s->cond) { /* NULL = forever, left by 'break' */
                check_expr(u, f, sc, s->cond);
                if (ty_is_complex(s->cond->ty) && cx_lowering())
                s->cond = cx_truth(s->cond);
            else
                need_scalar(u, s->cond, "'for'");
            }
            if (s->step)
                check_expr(u, f, sc, s->step);
            check_stmt(u, f, sc, s->body, 1, in_switch, 0);
            for (int i = mark; i < sc->n; i++)
                sc->vars[i].active = 0;
            sc->block_start = prev;
            break;
        }
        case STMT_BLOCK: {
            int mark = sc->n, prev = sc->block_start;
            sc->block_start = mark;
            check_stmt(u, f, sc, s->body, in_loop, in_switch, 0);
            for (int i = mark; i < sc->n; i++)
                sc->vars[i].active = 0; /* the block closed */
            sc->block_start = prev;
            break;
        }
        case STMT_LABEL:
            /* the labeled statement is checked in the label's own context */
            check_stmt(u, f, sc, s->body, in_loop, in_switch, 0);
            break;
        case STMT_EHREGION:
            check_stmt(u, f, sc, s->body, in_loop, in_switch, 0);
            check_expr(u, f, sc, s->expr);
            check_expr(u, f, sc, s->cond);
            if (!is_lvalue(s->expr) || s->expr->ty->kind != TY_PTR)
                sema_error_at(u, s->line, s->col, "a landing pad's exception "
                        "pointer must be a pointer lvalue");
            if (!is_lvalue(s->cond) || !ty_is_integer(s->cond->ty) ||
                ty_size(s->cond->ty) != 8)
                sema_error_at(u, s->line, s->col, "a landing pad's selector "
                        "must be a long lvalue");
            for (int i = 0; i < s->neh_acts; i++) {
                struct eh_act *a = &s->eh_acts[i];
                if (!a->name)
                    continue;
                a->ti = find_global(u, a->name);
                if (!a->ti)
                    sema_error_at(u, s->line, s->col, "'%s' is not a declared "
                            "typeinfo object", a->name);
                a->ti->used = 1;
            }
            check_stmt(u, f, sc, s->thn, in_loop, in_switch, 0);
            break;
        case STMT_GOTO:
            /* target existence is validated function-wide at codegen */
            if (s->expr) {   /* computed goto `goto *expr` (GNU) */
                check_expr(u, f, sc, s->expr);
                if (s->expr->ty->kind != TY_PTR)
                    sema_error_at(u, s->line, s->col,
                            "computed goto ('goto *') needs a pointer operand");
            }
            break;
        }
    }
    g_recover = save;
}

/* Conservative all-paths-return. Refusing a maybe-missing return is
 * honest; miscompiling one is not (THE RULE) — but "conservative" must
 * not mean "wrong about ordinary code", so switch and infinite loops
 * are analysed rather than assumed to fall through. */
static int list_returns(struct stmt *s);

/* Does a break leave THIS construct? Breaks inside a nested loop or
 * switch belong to that one, so they do not count. */
static int has_own_break(struct stmt *s)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_BREAK:
            return 1;
        case STMT_BLOCK:
            if (has_own_break(s->body))
                return 1;
            break;
        case STMT_IF:
            if (has_own_break(s->thn) ||
                (s->els && has_own_break(s->els)))
                return 1;
            break;
        case STMT_EHREGION:
            if (has_own_break(s->body) || has_own_break(s->thn))
                return 1;
            break;
        default:
            break; /* a nested loop/switch captures its own breaks */
        }
    }
    return 0;
}

/* Functions that never return, so a call to one terminates control flow
 * as surely as `return`. The noreturn attribute newlib puts on exit/abort
 * is stripped before EmbCC sees it (EmbCC does not parse __attribute__,
 * and _ATTRIBUTE expands empty for a non-GNU compiler), so the set is
 * recognized by name — sound because these genuinely never return, and
 * the worst case of a same-named user function is a missed diagnostic,
 * never a miscompile. diag_fatal is EmbCC's own, used at many tails. */
int is_noreturn_call(const struct expr *e)
{
    if (e->kind != EXPR_CALL || !e->name)
        return 0;
    if (e->callee && e->callee->is_noreturn)
        return 1;                       /* __attribute__((noreturn)) callee */
    static const char *const nr[] = {
        "exit", "abort", "_Exit", "diag_fatal",
        "__builtin_unreachable", "__builtin_trap",
    };
    for (unsigned i = 0; i < sizeof nr / sizeof *nr; i++)
        if (strcmp(e->name, nr[i]) == 0)
            return 1;
    return 0;
}

static int stmt_returns(struct stmt *s)
{
    switch (s->kind) {
    case STMT_RETURN:
        return 1;
    case STMT_EXPR:
        return s->expr && is_noreturn_call(s->expr);
    case STMT_BLOCK:
        return list_returns(s->body);
    case STMT_IF:
        return s->els && stmt_returns(s->thn) && stmt_returns(s->els);
    case STMT_EHREGION:          /* either way, returning */
        return stmt_returns(s->body) && stmt_returns(s->thn);
    case STMT_SWITCH: {
        /* Sound: with a default every value matches something, and with
         * no break the only way out is falling off the end — which the
         * last statement returning rules out. Anything reached earlier
         * either returns or falls through toward it. */
        struct stmt *list = switch_stmts(s->body);
        int has_default = 0;
        struct stmt *last = NULL;
        for (struct stmt *a = list; a; a = a->next) {
            if (a->kind == STMT_DEFAULT)
                has_default = 1;
            last = a;
        }
        if (!has_default || has_own_break(list) || !last)
            return 0;
        return stmt_returns(last);
    }
    case STMT_FOR:
        /* `for (;;)` with no break of its own never exits normally. */
        return !s->cond && !has_own_break(s->body);
    case STMT_WHILE: {
        long v;
        return const_fold(s->cond, &v) && v != 0 &&
               !has_own_break(s->body);
    }
    case STMT_DO: {
        long v;
        return (const_fold(s->cond, &v) && v != 0 &&
                !has_own_break(s->body)) || stmt_returns(s->body);
    }
    case STMT_GOTO:
        /* an unconditional jump: control leaves here, it does not fall off
         * the end of the function at this point. */
        return 1;
    case STMT_LABEL:
        return stmt_returns(s->body);
    default:
        return 0;
    }
}

static int list_returns(struct stmt *s)
{
    for (; s; s = s->next)
        if (stmt_returns(s))
            return 1;
    return 0;
}

static void check_func(struct unit *u, struct func *f)
{
    g_nundeclared = 0;           /* a fresh function: report its names again */
    const char *savefile = g_file;
    g_file = f->file ? f->file : u->file;
    struct scope sc = { 0, 0, 0, 0 };
    g_cx_sc = &sc;       /* complex lowering adds its temps here */

    for (int i = 0; i < f->nparams; i++) {
        if (scope_find(&sc, f->params[i]) >= 0)
            sema_error_line(u, f->line,
                       "duplicate parameter '%s' in '%s'",
                       f->params[i], f->name);
        {
            int pi = scope_add(&sc, f->params[i], f->param_tys[i], NULL);
            sc.vars[pi].is_param = 1;
            sc.vars[pi].line = f->line;
        }
    }
    /* `int a[n][m]` arrives as int (*)[m]: its row size is computed at
     * entry from the parameters before it */
    for (int i = 0; i < f->nparams; i++)
        if (ty_is_vm(f->param_tys[i])) {
            vla_prepare(u, f, &sc, f->param_tys[i]);
            f->has_vm_params = 1;
        }

    check_stmt(u, f, &sc, f->body, 0, 0, 0);

    if (f->ret_ty->kind != TY_VOID && !list_returns(f->body)) {
        diag_error_at(f->file ? f->file : u->file, f->line, 0,
                      "control may reach the end of '%s' — every path must "
                      "end in a return statement", f->name);
        diag_set_id("E0008");
        g_sema_errors++;
        if (g_recover)
            longjmp(*g_recover, 1);
        exit(1);
    }

    /* -Wunused-variable / -Wunused-parameter: nothing read it and nothing
     * wrote it, in the whole function. A name the compiler invented for
     * its own lowering (<vla size>, <compound literal>) is not the
     * programmer's and is never reported. */
    for (int i = 0; i < sc.n; i++) {
        struct vardef *v = &sc.vars[i];
        if (v->used || !v->name || v->name[0] == '<' || !v->line)
            continue;
        /* Names the C++ lowering invents (`this`, __cx_*) are EmbCC's, not
         * the programmer's: reporting them would be reporting ourselves. */
        if (!strcmp(v->name, "this") || !strncmp(v->name, "__cx_", 5))
            continue;
        if (v->is_param)
            diag_warn_opt(diag_file(u), v->line, v->col, "unused-parameter",
                          "unused parameter '%s'", v->name);
        else
            diag_warn_opt(diag_file(u), v->line, v->col, "unused-variable",
                          "unused variable '%s'", v->name);
    }

    /* -Wuninitialized: a dataflow question, so it gets its own pass over
     * the body — with the scope table still alive, since its indices ARE
     * the frame slots the walk reasons about (uninit.c). */
    if (sc.n > 0) {
        struct uninit_var *uv = xmalloc((size_t)sc.n * sizeof *uv);
        for (int i = 0; i < sc.n; i++) {
            uv[i].name = sc.vars[i].name;
            uv[i].ty = sc.vars[i].ty;
            uv[i].is_param = sc.vars[i].is_param;
            uv[i].is_static = sc.vars[i].g != NULL ||
                              sc.vars[i].asm_reg != NULL;
            uv[i].line = sc.vars[i].line;
            uv[i].col = sc.vars[i].col;
        }
        uninit_check(diag_file(u), f, uv, sc.n);
        free(uv);
    }

    f->nvars = sc.n;
    f->var_tys = xmalloc((size_t)(sc.n ? sc.n : 1) * sizeof *f->var_tys);
    f->var_aligns = xmalloc((size_t)(sc.n ? sc.n : 1) * sizeof *f->var_aligns);
    for (int i = 0; i < sc.n; i++) {
        /* a static local keeps its scope index but needs no frame
         * storage — give it a pointer's worth and never address it */
        f->var_tys[i] = sc.vars[i].g ? ty_base(TY_LONG, 0)
                                     : sc.vars[i].ty;
        /* a VLA's slot holds the pointer to its run-time storage */
        if (ty_is_vla(f->var_tys[i]))
            f->var_tys[i] = ty_ptr(f->var_tys[i]->pointee);
        f->var_aligns[i] = sc.vars[i].g ? 0 : sc.vars[i].user_align;
    }
    free(sc.vars);
    g_cx_sc = NULL;
    g_file = savefile;
}

/* Merge every later declaration of a name into its first (canonical)
 * node. C's static rule kept exactly: static-then-non-static keeps
 * internal linkage, non-static-then-static is an error (gcc agrees). */
static void merge_decls(struct unit *u)
{
    for (struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed)
            continue;
        struct func *canon = find_func(u, f->name);
        if (canon == f) {
            f->has_defn = f->defined;
            continue;
        }
        int match = canon->nparams == f->nparams &&
                    canon->is_varargs == f->is_varargs &&
                    ty_equal(canon->ret_ty, f->ret_ty);
        for (int i = 0; match && i < f->nparams; i++)
            if (!ty_equal(canon->param_tys[i], f->param_tys[i]))
                match = 0;
        if (!match) {
            diag_error_at(f->file, f->line, 0,
                          "conflicting declaration of '%s'", f->name);
            diag_note_at(canon->file, canon->line, 0,
                         "previous declaration of '%s' here", f->name);
            exit(1);
        }
        if (f->is_static && !canon->is_static)
            sema_error_line(u, f->line,
                       "static declaration of '%s' follows non-static "
                       "declaration (line %d)", f->name, canon->line);
        if (f->defined) {
            if (canon->has_defn) {
                diag_error_at(f->file, f->line, 0, "redefinition of '%s'",
                              f->name);
                diag_note_at(canon->file, canon->line, 0,
                             "previous definition of '%s' here", f->name);
                exit(1);
            }
            canon->has_defn = 1;
            canon->body = f->body;
            /* The canonical node now IS the definition, so it must carry
             * where the definition is. Otherwise every diagnostic about the
             * body -- an unused local, a missing return, a variable used
             * uninitialized -- is reported against the prototype's file,
             * which for a function declared in a header is the header, with
             * the .c file's line numbers. */
            canon->file = f->file;
            canon->line = f->line;
            for (int i = 0; i < f->nparams; i++)
                canon->params[i] = f->params[i]; /* definition names win */
        }
        canon->is_weak |= f->is_weak;  /* weak on any declaration is weak */
        canon->sret_first |= f->sret_first;
        canon->is_noreturn |= f->is_noreturn;  /* noreturn on any wins */
        canon->is_nothrow |= f->is_nothrow;
        f->absorbed = 1;
    }
}

/* Merge later declarations of each global into its canonical node.
 * `int g;` counts as a definition (the tentative-definition subtlety is
 * collapsed); extern declares without defining; at most one
 * initializer. Same static linkage rules as functions. */
static void merge_globals(struct unit *u)
{
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed)
            continue;
        struct global *canon = find_global(u, g->name);
        if (find_func(u, g->name))
            sema_error_line(u, g->line,
                       "'%s' is declared as both a function and a "
                       "variable", g->name);
        if (canon == g) {
            g->defined = !g->is_extern;
            continue;
        }
        /* Two declarations of an array are compatible when their element
         * types match and at most one gives a size — `extern T x[];`
         * completed by `T x[N] = …`. The canonical node adopts the
         * complete type so its symbol carries the real size. */
        int compat = ty_equal(canon->ty, g->ty);
        if (!compat && canon->ty->kind == TY_ARRAY &&
            g->ty->kind == TY_ARRAY &&
            ty_equal(canon->ty->pointee, g->ty->pointee) &&
            (canon->ty->count == 0 || g->ty->count == 0 ||
             canon->ty->count == g->ty->count)) {
            compat = 1;
            if (canon->ty->count == 0)
                canon->ty = g->ty;
        }
        if (!compat) {
            diag_error_at(g->file, g->line, 0,
                          "conflicting types for '%s': %s here, %s before",
                          g->name, ty_name(g->ty), ty_name(canon->ty));
            diag_note_at(canon->file, canon->line, 0,
                         "previous declaration of '%s' here", g->name);
            exit(1);
        }
        if (g->is_static && !canon->is_static)
            sema_error_line(u, g->line,
                       "static declaration of '%s' follows non-static "
                       "declaration (line %d)", g->name, canon->line);
        if (g->has_init) {
            if (canon->has_init) {
                diag_error_at(g->file, g->line, 0, "redefinition of '%s'",
                              g->name);
                diag_note_at(canon->file, canon->line, 0,
                             "previous definition of '%s' here", g->name);
                exit(1);
            }
            canon->has_init = 1;
            canon->init = g->init;
            canon->init_expr = g->init_expr;
            canon->def_seq = g->seq;   /* the initializer's real position */
        }
        canon->defined |= !g->is_extern;
        canon->is_weak |= g->is_weak;
        if (g->section) {
            if (canon->section && strcmp(canon->section, g->section) != 0) {
                diag_error_at(g->file, g->line, 0,
                              "'%s' placed in section '%s' after section "
                              "'%s'", g->name, g->section, canon->section);
                diag_note_at(canon->file, canon->line, 0,
                             "previous declaration of '%s' here", g->name);
                exit(1);
            }
            canon->section = g->section;
        }
        g->absorbed = 1;
    }
}

void sema_check(struct unit *u)
{
    merge_decls(u);
    merge_globals(u);
    lower_globals(u);

    /* Walk in source order so `declared` mirrors C's rule exactly: a
     * name is usable from its first declaration on, and a body is
     * checked at its DEFINITION's position. */
    for (struct func *f = u->funcs; f; f = f->next) {
        struct func *canon = find_func(u, f->name);
        if (canon == f)
            f->declared = 1;
        if (f->defined) {
            cur_body_seq = f->seq;
            check_func(u, canon);
        }
    }

    /* -Wunused-function: a static nobody calls is dead in this unit, and
     * no other unit can reach it. (A non-static one may be called from
     * anywhere, so it is never reported.) */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn && f->is_static && !f->used &&
            f->name && strcmp(f->name, "main") != 0)
            diag_warn_opt(f->file ? f->file : u->file, f->line, 0,
                          "unused-function", "unused function '%s'", f->name);

    /* An undefined non-static is an external: the linker gets a chance.
     * An undefined static has no linker to save it — refuse now instead
     * of emitting an unresolvable object (THE RULE). */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && !f->has_defn && f->is_static && f->used)
            sema_error_line(u, f->line,
                       "static function '%s' is called but never defined",
                       f->name);
}
