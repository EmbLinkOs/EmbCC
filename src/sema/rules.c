/* The coding-standard checker: the MISRA C:2023 and CERT C rules of
 * rules.def, over a unit sema has checked. Each finding is a warning
 * named after its rule, so it is turned on, off, made an error and
 * silenced by pragma like any other; --misra and --cert turn on a whole
 * standard. Nothing here changes the program.
 *
 * MISRA's essential type model is approximated where EmbCC's tree
 * allows: an expression's essential type is its type with the implicit
 * conversions sema inserted taken off (their column is 0), a comparison
 * or a logical operator's is Boolean, an integer constant's category is
 * its sign and its width the fewest bytes that hold it. Three rules are
 * checked where the tree still has what they read: 7.1-7.3 (a constant's
 * spelling) by the parser, 8.14 (restrict, which the types forget) by
 * the parser, 13.6 (sizeof's operand, which sema folds away) by sema. */

#include "sema.h"
#include "type.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../driver/util.h"

struct rule { const char *name, *std, *id, *cat, *text; };
static const struct rule g_rules[] = {
#define RULE(n, s, i, c, t) { n, s, i, c, t },
#include "rules.def"
#undef RULE
};
#define NRULES ((int)(sizeof g_rules / sizeof g_rules[0]))

static int g_any;                      /* some rule is on */
static const char *g_file;
static struct func *g_fn;
static struct unit *g_unit;

/* a variable's declared type: sema gives the name of an array its
 * decayed pointer type, so the bound is the declaration's */
static const struct type *declared_type(const struct expr *v)
{
    if (!v || v->kind != EXPR_VAR)
        return NULL;
    if (v->undecayed)
        return v->undecayed;
    if (v->gref)
        return v->gref->ty;
    if (g_fn && v->var_index >= 0 && v->var_index < g_fn->nvars &&
        g_fn->var_tys && g_fn->var_tys[v->var_index])
        return g_fn->var_tys[v->var_index];
    for (struct global *g = g_unit ? g_unit->globals : NULL; g && v->name;
         g = g->next)
        if (!g->absorbed && !strcmp(g->name, v->name))
            return g->ty;
    return v->ty;
}

int rules_count(void) { return NRULES; }
const char *rules_name(int i) { return g_rules[i].name; }
const char *rules_text(int i) { return g_rules[i].text; }

static const struct rule *rule(const char *name)
{
    for (int i = 0; i < NRULES; i++)
        if (!strcmp(g_rules[i].name, name))
            return &g_rules[i];
    return NULL;
}

/* name's warning, at file:line:col, in the rule's words and then the
 * detail */
void rules_report(const char *file, int line, int col, const char *name,
                  const char *detail)
{
    const struct rule *r = rule(name);
    if (!r || !diag_warning_enabled(name))
        return;
    if (detail && *detail)
        diag_warn_opt(file, line, col, name, "%s %s (%s): %s: %s", r->std,
                      r->id, r->cat, r->text, detail);
    else
        diag_warn_opt(file, line, col, name, "%s %s (%s): %s", r->std,
                      r->id, r->cat, r->text);
}

#define R(name, e, detail) \
    rules_report(g_file, (e)->line, (e)->col, name, detail)

/* ---- side effects and the essential type ------------------------------ */

int rules_side_effects(const struct expr *e)
{
    if (!e)
        return 0;
    switch (e->kind) {
    case EXPR_ASSIGN: case EXPR_COMPOUND: case EXPR_INCDEC: case EXPR_CALL:
    case EXPR_STMTEXPR: case EXPR_VA_ARG:
        return 1;
    case EXPR_VAR: case EXPR_DEREF: case EXPR_MEMBER:
        if (e->ty && e->ty->is_volatile)
            return 1;
        break;
    default:
        break;
    }
    if (rules_side_effects(e->lhs) || rules_side_effects(e->rhs))
        return 1;
    for (int k = 0; e->args && k < e->nargs; k++)
        if (rules_side_effects(e->args[k]))
            return 1;
    return 0;
}

static int implicit_cast(const struct expr *e)
{
    return e && e->kind == EXPR_CAST && e->col == 0;
}

static const struct expr *strip(const struct expr *e)
{
    while (implicit_cast(e) && e->rhs)
        e = e->rhs;
    return e;
}

enum ess { E_NONE, E_BOOL, E_CHAR, E_SIGNED, E_UNSIGNED, E_ENUM, E_FLOAT,
           E_PTR };

static int is_bool_op(const struct expr *e)
{
    if (e->kind == EXPR_NOT)
        return 1;
    if (e->kind != EXPR_BINOP)
        return 0;
    switch (e->op) {
    case B_EQ: case B_NE: case B_LT: case B_LE: case B_GT: case B_GE:
    case B_LAND: case B_LOR:
        return 1;
    default:
        return 0;
    }
}

static enum ess ess_of(const struct expr *e0)
{
    const struct expr *e = strip(e0);
    if (!e || !e->ty)
        return E_NONE;
    if (is_bool_op(e))
        return E_BOOL;
    const struct type *t = e->ty;
    if (t->kind == TY_BOOL)
        return E_BOOL;
    if (t->kind == TY_PTR || t->kind == TY_ARRAY)
        return E_PTR;
    if (t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE ||
        t->kind == TY_FLOAT16)
        return E_FLOAT;
    if (t->is_enum && e->kind != EXPR_NUM)
        return E_ENUM;
    if (ty_is_plain_char(t))
        return E_CHAR;
    if (ty_is_integer(t))
        return e->kind == EXPR_NUM && e->num >= 0 && !t->is_unsigned
               ? E_SIGNED : t->is_unsigned ? E_UNSIGNED : E_SIGNED;
    return E_NONE;
}

/* the essential width in bytes: a constant's fewest, an operator's the
 * wider of its operands' (the C promotion taken off) */
static int ess_width(const struct expr *e0)
{
    const struct expr *e = strip(e0);
    if (!e || !e->ty)
        return 0;
    if (e->kind == EXPR_NUM) {
        long v = e->num < 0 ? -(e->num + 1) : e->num;
        int signed_ = !e->ty->is_unsigned;
        int w = 1;
        while (w < 8 && (unsigned long)v >> (8 * w - signed_))
            w++;
        return w;
    }
    if (e->kind == EXPR_BINOP && !is_bool_op(e) && e->op != B_SHL &&
        e->op != B_SHR) {
        int a = ess_width(e->lhs), b = ess_width(e->rhs);
        return a > b ? a : b;
    }
    if (e->kind == EXPR_BINOP && (e->op == B_SHL || e->op == B_SHR))
        return ess_width(e->lhs);
    if (e->kind == EXPR_NEG || e->kind == EXPR_BNOT)
        return ess_width(e->rhs);
    if (e->kind == EXPR_COND)
        return ess_width(e->lhs) > ess_width(e->rhs) ? ess_width(e->lhs)
                                                     : ess_width(e->rhs);
    return ty_size(e->ty);
}

static int composite(const struct expr *e0)
{
    const struct expr *e = strip(e0);
    if (!e)
        return 0;
    if (e->kind == EXPR_BINOP)
        return !is_bool_op(e) && !e->is_subscript;
    return e->kind == EXPR_NEG || e->kind == EXPR_BNOT ||
           (e->kind == EXPR_COND && (composite(e->lhs) || composite(e->rhs)));
}

static int is_const_expr(const struct expr *e, long *v)
{
    return sema_const_fold(e, v);
}

/* ---- the walk ---------------------------------------------------------- */

struct ctx {
    int in_cond;                /* a controlling expression */
    int used;                   /* the value is used */
};

static void walk_expr(struct expr *e, int used);
static void walk_stmt(struct stmt *s);

static int is_string(const struct expr *e)
{
    e = strip(e);
    return e && e->kind == EXPR_STR;
}

static int char_ptr_nonconst(const struct type *t)
{
    return t && t->kind == TY_PTR && t->pointee &&
           t->pointee->kind == TY_CHAR && !t->pointee->is_const;
}

/* 10.3, 10.6, 7.4, STR30: a value given to an object of type `to` */
static void assigned(const struct expr *v, const struct type *to,
                     const struct expr *at)
{
    if (!v || !to)
        return;
    at = strip(at);
    if (is_string(v) && char_ptr_nonconst(to)) {
        R("misra-7.4", at, "");
        R("cert-str30", at, "");
    }
    if (!implicit_cast(v))
        return;
    const struct expr *src = strip(v);
    enum ess se = ess_of(src);
    struct expr tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.kind = EXPR_VAR;
    tmp.ty = (struct type *)to;
    enum ess te = ess_of(&tmp);
    if (se == E_NONE || te == E_NONE || se == E_PTR || te == E_PTR)
        return;
    long cv;
    int k = src->kind == EXPR_NUM || is_const_expr(src, &cv);
    if (src->kind == EXPR_NUM)
        cv = src->num;
    /* a non-negative constant that fits is allowed into either sign */
    if (k && (te == E_SIGNED || te == E_UNSIGNED) &&
        (se == E_SIGNED || se == E_UNSIGNED) && cv >= 0 &&
        ess_width(src) <= ty_size(to))
        return;
    if (se != te && !(se == E_CHAR && te == E_CHAR)) {
        char d[96];
        snprintf(d, sizeof d, "%s to %s",
                 se == E_BOOL ? "Boolean" : se == E_CHAR ? "character"
                 : se == E_SIGNED ? "signed" : se == E_UNSIGNED ? "unsigned"
                 : se == E_ENUM ? "enum" : "floating",
                 te == E_BOOL ? "Boolean" : te == E_CHAR ? "character"
                 : te == E_SIGNED ? "signed" : te == E_UNSIGNED ? "unsigned"
                 : te == E_ENUM ? "enum" : "floating");
        R("misra-10.3", at, d);
    } else if (ess_width(src) > ty_size(to)) {
        R("misra-10.3", at, "to a narrower type");
    } else if (composite(src) && ess_width(src) < ty_size(to)) {
        R("misra-10.6", at, "");
    }
}

static int prec(const struct expr *e)
{
    if (e->kind != EXPR_BINOP)
        return 0;
    switch (e->op) {
    case B_MUL: case B_DIV: case B_MOD: return 10;
    case B_ADD: case B_SUB: return 9;
    case B_SHL: case B_SHR: return 8;
    case B_LT: case B_LE: case B_GT: case B_GE: return 7;
    case B_EQ: case B_NE: return 6;
    case B_AND: return 5;
    case B_XOR: return 4;
    case B_OR: return 3;
    case B_LAND: return 2;
    case B_LOR: return 1;
    }
    return 0;
}

static int arith(int p) { return p >= 9; }

static void binop_rules(struct expr *e)
{
    const struct expr *l = strip(e->lhs), *r = strip(e->rhs);
    enum ess le = ess_of(e->lhs), re = ess_of(e->rhs);
    int p = prec(e);
    /* 10.1 */
    switch (e->op) {
    case B_ADD: case B_SUB: case B_MUL: case B_DIV: case B_MOD:
    case B_LT: case B_LE: case B_GT: case B_GE:
        if (le == E_BOOL || re == E_BOOL)
            R("misra-10.1", e, "a Boolean operand of arithmetic");
        else if ((le == E_ENUM || re == E_ENUM) && p >= 9)
            R("misra-10.1", e, "an enum operand of arithmetic");
        break;
    case B_AND: case B_OR: case B_XOR:
        if (le == E_BOOL || re == E_BOOL) {
            R("misra-10.1", e, "a Boolean operand of a bitwise operator");
            R("cert-exp46", e, "");
        } else if ((le == E_SIGNED && l->kind != EXPR_NUM) ||
                   (re == E_SIGNED && r->kind != EXPR_NUM))
            R("misra-10.1", e, "a signed operand of a bitwise operator");
        break;
    case B_SHL: case B_SHR:
        if (le == E_SIGNED && l->kind != EXPR_NUM)
            R("misra-10.1", e, "a signed left operand of a shift");
        if (le == E_BOOL || re == E_BOOL)
            R("misra-10.1", e, "a Boolean operand of a shift");
        break;
    case B_LAND: case B_LOR:
        if ((le != E_BOOL && le != E_NONE) || (re != E_BOOL && re != E_NONE))
            R("misra-10.1", e, "an operand of && or || that is not Boolean");
        if (rules_side_effects(e->rhs))
            R("misra-13.5", e, "");
        break;
    default:
        break;
    }
    /* 10.4: the usual arithmetic conversions between categories */
    if (p >= 3 && p != 8 && !e->is_subscript && le != E_PTR && re != E_PTR &&
        le != E_NONE && re != E_NONE && le != E_BOOL && re != E_BOOL) {
        int ls = le == E_SIGNED || le == E_CHAR, rs = re == E_SIGNED || re == E_CHAR;
        if ((le == E_FLOAT) != (re == E_FLOAT) ||
            (le != E_FLOAT && re != E_FLOAT && ls != rs &&
             !(le == E_CHAR && re == E_CHAR)))
            R("misra-10.4", e, "");
    }
    /* 12.1: mixed precedences without parentheses (arithmetic inside
     * arithmetic excepted) */
    const struct expr *sides[2] = { e->lhs, e->rhs };
    for (int k = 0; k < 2; k++) {
        const struct expr *o = strip(sides[k]);
        if (o && o->kind == EXPR_BINOP && !o->parens && !o->is_subscript &&
            prec(o) != p && !(arith(p) && arith(prec(o))) &&
            !((p == 2 || p == 1) && prec(o) <= 2 && prec(o) == p))
            R("misra-12.1", o, "");
    }
    /* 12.2 and INT34: a constant shift count */
    long c;
    if ((e->op == B_SHL || e->op == B_SHR) && r && is_const_expr(r, &c)) {
        int w = 8 * ess_width(e->lhs);
        if (l && l->kind == EXPR_NUM)
            w = 8 * ty_size(e->lhs->ty);
        if (c < 0 || c >= w)
            R("misra-12.2", e, "");
    }
    /* 18.4: pointer arithmetic */
    if ((e->op == B_ADD || e->op == B_SUB) && !e->is_subscript &&
        ((le == E_PTR && re != E_PTR) || (re == E_PTR && le != E_PTR)))
        R("misra-18.4", e, "");
    /* ARR30: a constant subscript past a known array */
    const struct type *at = l ? declared_type(l) : NULL;
    if (e->is_subscript && at && at->kind == TY_ARRAY && at->count > 0 && r &&
        is_const_expr(r, &c) && (c < 0 || c >= at->count)) {
        char d[64];
        snprintf(d, sizeof d, "index %ld of %d elements", c, at->count);
        R("cert-arr30", e, d);
    }
}

/* 11.x: a conversion from the type of `from` to `to` */
static void conversion(const struct expr *at, const struct type *to,
                       const struct expr *from, int explicit_)
{
    if (!from || !from->ty || !to)
        return;
    const struct type *ft = from->ty;
    if (ft->kind == TY_ARRAY || ft->kind == TY_FUNC)
        return;                       /* decay, not a conversion of values */
    int fp = ft->kind == TY_PTR, tp = to->kind == TY_PTR;
    if (!fp && !tp)
        return;
    const struct type *fpt = fp ? ft->pointee : NULL, *tpt = tp ? to->pointee : NULL;
    int ffn = fpt && fpt->kind == TY_FUNC, tfn = tpt && tpt->kind == TY_FUNC;
    long v;
    int nullc = !fp && is_const_expr(from, &v) && v == 0;
    if ((ffn || tfn) && !nullc && !(ffn && tfn && fpt == tpt) &&
        !(to->kind == TY_VOID))
        R("misra-11.1", at, "");
    if (nullc && tp && !explicit_ && strip(from)->kind == EXPR_NUM)
        R("misra-11.9", at, "");
    if ((fp && ty_is_integer(to) && to->kind != TY_BOOL) ||
        (tp && ty_is_integer(ft) && !nullc))
        R("misra-11.4", at, "");
    int fvoid = fpt && fpt->kind == TY_VOID, tvoid = tpt && tpt->kind == TY_VOID;
    if (fvoid && tp && !tvoid && !tfn)
        R("misra-11.5", at, "");
    if (explicit_ && ((fvoid && !tp && to->kind != TY_VOID) ||
                      (tvoid && !fp && !nullc)))
        R("misra-11.6", at, "");
    int tflt = to->kind == TY_FLOAT || to->kind == TY_DOUBLE ||
               to->kind == TY_BOOL || to->kind == TY_LDOUBLE;
    int fflt = ft->kind == TY_FLOAT || ft->kind == TY_DOUBLE ||
               ft->kind == TY_BOOL || ft->kind == TY_LDOUBLE;
    if (explicit_ && ((fp && tflt) || (tp && fflt)))
        R("misra-11.7", at, "");
    if (explicit_ && fp && tp && fpt && tpt && !ffn && !tfn && !fvoid &&
        !tvoid && tpt->kind != TY_CHAR &&
        ty_size(fpt) > 0 && (fpt->kind != tpt->kind ||
                             ty_size(fpt) != ty_size(tpt) ||
                             (fpt->kind == TY_STRUCT && fpt->members != tpt->members)))
        R("misra-11.3", at, "");
    if (fp && tp && fpt && tpt &&
        ((fpt->is_const && !tpt->is_const) ||
         (fpt->is_volatile && !tpt->is_volatile)))
        R("misra-11.8", at, "");
}

static const char *callee_name(const struct expr *e)
{
    if (e->callee && e->callee->name)
        return e->callee->name;
    if (e->lhs && e->lhs->kind == EXPR_VAR)
        return e->lhs->name;
    return e->name;
}

static int name_in(const char *n, const char *const *list)
{
    for (int k = 0; n && list[k]; k++)
        if (!strcmp(n, list[k]))
            return 1;
    return 0;
}

static void call_rules(struct expr *e, int used)
{
    const char *n = callee_name(e);
    if (!n)
        return;
    static const char *const alloc[] = { "malloc", "calloc", "realloc", "free",
        "aligned_alloc", NULL };
    static const char *const sj[] = { "setjmp", "longjmp", "_setjmp",
        "_longjmp", "sigsetjmp", "siglongjmp", NULL };
    static const char *const sig[] = { "signal", "raise", NULL };
    static const char *const io[] = { "printf", "fprintf", "sprintf",
        "snprintf", "vprintf", "vfprintf", "vsprintf", "vsnprintf", "scanf",
        "fscanf", "sscanf", "vscanf", "vfscanf", "vsscanf", "fopen", "freopen",
        "fclose", "fflush", "fread", "fwrite", "fgetc", "fgets", "fputc",
        "fputs", "getc", "getchar", "gets", "putc", "putchar", "puts", "ungetc",
        "fseek", "ftell", "rewind", "fgetpos", "fsetpos", "clearerr", "feof",
        "ferror", "perror", "remove", "rename", "tmpfile", "tmpnam", "setbuf",
        "setvbuf", NULL };
    static const char *const ato[] = { "atof", "atoi", "atol", "atoll", NULL };
    static const char *const term[] = { "abort", "exit", "_Exit", "quick_exit",
        NULL };
    static const char *const sortf[] = { "bsearch", "qsort", NULL };
    static const char *const tm[] = { "time", "clock", "difftime", "mktime",
        "asctime", "ctime", "gmtime", "localtime", "strftime", "timespec_get",
        NULL };
    static const char *const fe[] = { "feclearexcept", "fegetexceptflag",
        "feraiseexcept", "fesetexceptflag", "fetestexcept", NULL };
    static const char *const err33[] = { "malloc", "calloc", "realloc",
        "aligned_alloc", "fopen", "freopen", "fgets", "fread", "fwrite",
        "fscanf", "scanf", "sscanf", "fseek", "ftell", "remove", "rename",
        "tmpfile", "setvbuf", "signal", "mktime", "strtol", "strtoul",
        "strtod", NULL };
    static const char *const msc24[] = { "gets", "atof", "atoi", "atol",
        "atoll", "setbuf", "rewind", "asctime", "ctime", NULL };
    if (name_in(n, alloc)) R("misra-21.3", e, n);
    if (name_in(n, sj)) R("misra-21.4", e, n);
    if (name_in(n, sig)) R("misra-21.5", e, n);
    if (name_in(n, io)) R("misra-21.6", e, n);
    if (name_in(n, ato)) R("misra-21.7", e, n);
    if (name_in(n, term)) R("misra-21.8", e, n);
    if (name_in(n, sortf)) R("misra-21.9", e, n);
    if (name_in(n, tm)) R("misra-21.10", e, n);
    if (name_in(n, fe)) R("misra-21.12", e, n);
    if (!strcmp(n, "system")) {
        R("misra-21.21", e, "");
        R("cert-env33", e, "");
    }
    if (!strcmp(n, "rand") || !strcmp(n, "srand"))
        R("misra-21.24", e, n);
    if (!strcmp(n, "rand"))
        R("cert-msc30", e, "");
    if (!strcmp(n, "gets"))
        R("cert-str31", e, "");
    if (name_in(n, msc24))
        R("cert-msc24", e, n);
    if (!strncmp(n, "__builtin_va_", 13) || !strcmp(n, "va_start") ||
        !strcmp(n, "va_end") || !strcmp(n, "va_copy"))
        R("misra-17.1", e, "");
    if (!used && name_in(n, err33))
        R("cert-err33", e, n);
    if (!strcmp(n, "memcmp") && e->nargs >= 2)
        for (int k = 0; k < 2; k++) {
            const struct expr *a = strip(e->args[k]);
            if (a && a->ty && a->ty->kind == TY_PTR && a->ty->pointee &&
                a->ty->pointee->kind == TY_STRUCT)
                R("cert-exp42", e, "");
        }
    if (!strcmp(n, "free") && e->nargs >= 1) {
        const struct expr *a = strip(e->args[0]);
        int bad = 0;
        if (a && a->kind == EXPR_ADDR)
            bad = 1;
        else if (a && a->kind == EXPR_VAR && a->ty && a->ty->kind == TY_ARRAY)
            bad = 1;
        if (bad)
            R("cert-mem34", e, "");
    }
}

/* EXP30: within one full expression, an object written twice, or
 * written by ++/--/op= and read elsewhere, outside &&, ||, ?: and the
 * comma (which sequence) */
struct seen { int v[64], w[64], n; };

static void exp30_walk(const struct expr *e, struct seen *s, int write,
                       int *bad)
{
    if (!e || *bad)
        return;
    if (e->kind == EXPR_BINOP && (e->op == B_LAND || e->op == B_LOR))
        return;                         /* sequenced */
    if (e->kind == EXPR_COMMA || e->kind == EXPR_COND || e->kind == EXPR_CALL ||
        e->kind == EXPR_STMTEXPR)
        return;
    if (e->kind == EXPR_VAR && e->var_index >= 0) {
        for (int k = 0; k < s->n; k++)
            if (s->v[k] == e->var_index &&
                (s->w[k] == 2 || (write && s->w[k]) || (write == 2)))
                *bad = 1;
        if (s->n < 64) {
            s->v[s->n] = e->var_index;
            s->w[s->n] = write;
            s->n++;
        }
        return;
    }
    if (e->kind == EXPR_INCDEC || e->kind == EXPR_COMPOUND) {
        exp30_walk(e->lhs, s, 2, bad);
        exp30_walk(e->rhs, s, 0, bad);
        return;
    }
    if (e->kind == EXPR_ASSIGN) {
        exp30_walk(e->rhs, s, 0, bad);
        if (e->lhs && e->lhs->kind == EXPR_VAR)
            exp30_walk(e->lhs, s, 1, bad);
        else
            exp30_walk(e->lhs, s, 0, bad);
        return;
    }
    exp30_walk(e->lhs, s, 0, bad);
    exp30_walk(e->rhs, s, 0, bad);
    for (int k = 0; e->args && k < e->nargs; k++)
        exp30_walk(e->args[k], s, 0, bad);
}

static int count_effects(const struct expr *e)
{
    if (!e)
        return 0;
    int n = (e->kind == EXPR_ASSIGN || e->kind == EXPR_COMPOUND ||
             e->kind == EXPR_INCDEC || e->kind == EXPR_CALL);
    n += count_effects(e->lhs) + count_effects(e->rhs);
    for (int k = 0; e->args && k < e->nargs; k++)
        n += count_effects(e->args[k]);
    return n;
}

static void full_expr(struct expr *e, int used)
{
    if (!e)
        return;
    struct seen s = { { 0 }, { 0 }, 0 };
    int bad = 0;
    exp30_walk(e, &s, 0, &bad);
    if (bad)
        R("cert-exp30", e, "");
    walk_expr(e, used);
}

static int incdec_inside(const struct expr *e)
{
    if (!e)
        return 0;
    if (e->kind == EXPR_INCDEC)
        return 1;
    if (incdec_inside(e->lhs) || incdec_inside(e->rhs))
        return 1;
    for (int k = 0; e->args && k < e->nargs; k++)
        if (incdec_inside(e->args[k]))
            return 1;
    return 0;
}

static void walk_expr(struct expr *e, int used)
{
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_BINOP:
        binop_rules(e);
        walk_expr(e->lhs, 1);
        walk_expr(e->rhs, 1);
        return;
    case EXPR_ASSIGN: case EXPR_COMPOUND:
        if (used)
            R("misra-13.4", e, "");
        if (e->kind == EXPR_ASSIGN)
            assigned(e->rhs, e->lhs ? e->lhs->ty : NULL, e);
        if (e->kind == EXPR_COMPOUND && e->lhs && e->lhs->ty &&
            e->lhs->ty->kind == TY_PTR && (e->op == B_ADD || e->op == B_SUB))
            R("misra-18.4", e, "");
        if (e->lhs && e->lhs->kind == EXPR_VAR && g_fn &&
            e->lhs->var_index >= 0 && e->lhs->var_index < g_fn->nparams)
            R("misra-17.8", e, e->lhs->name);
        if (count_effects(e->rhs) && incdec_inside(e->rhs))
            R("misra-13.3", e, "");
        walk_expr(e->lhs, 1);
        walk_expr(e->rhs, 1);
        return;
    case EXPR_INCDEC:
        if (used && count_effects(e) > 1)
            R("misra-13.3", e, "");
        if (e->lhs && e->lhs->kind == EXPR_VAR && g_fn &&
            e->lhs->var_index >= 0 && e->lhs->var_index < g_fn->nparams)
            R("misra-17.8", e, e->lhs->name);
        walk_expr(e->lhs, 1);
        return;
    case EXPR_CALL:
        call_rules(e, used);
        if (e->callee) {
            for (int k = 0; k < e->nargs && k < e->callee->nparams; k++)
                assigned(e->args[k], e->callee->param_tys[k], e->args[k]);
        }
        walk_expr(e->lhs, 1);
        for (int k = 0; k < e->nargs; k++)
            walk_expr(e->args[k], 1);
        return;
    case EXPR_COMMA:
        R("misra-12.3", e, "");
        walk_expr(e->lhs, 0);
        walk_expr(e->rhs, used);
        return;
    case EXPR_CAST:
        if (!implicit_cast(e)) {
            conversion(e, e->cast_ty ? e->cast_ty : e->ty, e->rhs, 1);
            if (composite(e->rhs) && e->ty && ty_is_integer(e->ty)) {
                enum ess se = ess_of(e->rhs);
                struct expr t;
                memset(&t, 0, sizeof t);
                t.kind = EXPR_VAR;
                t.ty = e->ty;
                if (ess_of(&t) != se || ty_size(e->ty) > ess_width(e->rhs))
                    R("misra-10.8", e, "");
            }
        } else {
            conversion(e->rhs, e->ty, e->rhs, 0);
        }
        walk_expr(e->rhs, used);
        return;
    case EXPR_NEG:
        if (ess_of(e->rhs) == E_UNSIGNED)
            R("misra-10.1", e, "unary minus of an unsigned operand");
        if (ess_of(e->rhs) == E_BOOL)
            R("misra-10.1", e, "unary minus of a Boolean");
        walk_expr(e->rhs, 1);
        return;
    case EXPR_BNOT:
        if (ess_of(e->rhs) == E_SIGNED || ess_of(e->rhs) == E_BOOL)
            R("misra-10.1", e, "~ of a signed or Boolean operand");
        walk_expr(e->rhs, 1);
        return;
    case EXPR_NOT: {
        enum ess s = ess_of(e->rhs);
        if (s != E_BOOL && s != E_NONE)
            R("misra-10.1", e, "! of an operand that is not Boolean");
        walk_expr(e->rhs, 1);
        return;
    }
    case EXPR_COND: {
        enum ess s = e->nargs ? ess_of(e->args[0]) : E_NONE;
        long v;
        if (e->nargs && s != E_BOOL && s != E_NONE)
            R("misra-14.4", e->args[0], "");
        if (e->nargs && is_const_expr(e->args[0], &v))
            R("misra-14.3", e->args[0], "");
        if (e->nargs)
            walk_expr(e->args[0], 1);
        walk_expr(e->lhs, used);
        walk_expr(e->rhs, used);
        return;
    }
    case EXPR_VA_ARG:
        R("misra-17.1", e, "");
        walk_expr(e->lhs, 1);
        return;
    case EXPR_STMTEXPR:
        walk_stmt(e->body);
        return;
    default:
        break;
    }
    walk_expr(e->lhs, 1);
    walk_expr(e->rhs, 1);
    for (int k = 0; e->args && k < e->nargs; k++)
        walk_expr(e->args[k], 1);
    for (int k = 0; e->elems && k < e->nelems; k++)
        walk_expr(e->elems[k], 1);
    for (int k = 0; e->inits && k < e->ninits; k++)
        walk_expr(e->inits[k].e, 1);
}

static int is_compound(const struct stmt *s)
{
    return s && s->kind == STMT_BLOCK;
}

static void cond_rules(struct expr *c, int loop)
{
    if (!c)
        return;
    enum ess s = ess_of(c);
    if (s != E_BOOL && s != E_NONE)
        R("misra-14.4", c, "");
    long v;
    if (is_const_expr(c, &v) && !(loop && v))
        R("misra-14.3", c, "");
}

/* the statements between a switch's labels: does each clause end in a
 * jump? fewer than two clauses? where is default? */
static int ends_in_jump(const struct stmt *s)
{
    if (!s)
        return 0;
    switch (s->kind) {
    case STMT_BREAK: case STMT_RETURN: case STMT_GOTO: case STMT_CONTINUE:
        return 1;
    case STMT_BLOCK: {
        const struct stmt *last = NULL;
        for (const struct stmt *k = s->body; k; k = k->next)
            last = k;
        return ends_in_jump(last);
    }
    case STMT_EXPR:
        return s->expr && s->expr->kind == EXPR_CALL && s->expr->callee &&
               s->expr->callee->is_noreturn;
    default:
        return 0;
    }
}

static void switch_rules(struct stmt *s)
{
    struct stmt *body = s->body && s->body->kind == STMT_BLOCK ? s->body->body
                                                               : s->body;
    int clauses = 0, has_default = 0, default_at = -1, n = 0;
    const struct stmt *prev = NULL;
    int open = 0;
    for (struct stmt *k = body; k; k = k->next) {
        if (k->kind == STMT_CASE || k->kind == STMT_DEFAULT) {
            if (open && prev && prev->kind != STMT_CASE &&
                prev->kind != STMT_DEFAULT && !ends_in_jump(prev))
                R("misra-16.3", k, "");
            if (!prev || (prev->kind != STMT_CASE && prev->kind != STMT_DEFAULT))
                clauses++;
            if (k->kind == STMT_DEFAULT) {
                has_default = 1;
                default_at = n;
            }
            open = 1;
            n++;
        }
        prev = k;
    }
    if (open && prev && prev->kind != STMT_CASE && prev->kind != STMT_DEFAULT &&
        !ends_in_jump(prev))
        R("misra-16.3", s, "the last clause");
    if (!has_default)
        R("misra-16.4", s, "");
    else if (default_at != 0 && default_at != n - 1)
        R("misra-16.5", s, "");
    if (clauses < 2)
        R("misra-16.6", s, "");
    if (s->cond && ess_of(s->cond) == E_BOOL)
        R("misra-16.7", s->cond, "");
}

static int loop_exits(const struct stmt *s, int depth)
{
    int n = 0;
    for (; s; s = s->next) {
        if (s->kind == STMT_BREAK && depth == 0)
            n++;
        if (s->kind == STMT_GOTO)
            n++;
        int inner = s->kind == STMT_WHILE || s->kind == STMT_FOR ||
                    s->kind == STMT_DO || s->kind == STMT_SWITCH;
        if (s->kind == STMT_BLOCK)
            n += loop_exits(s->body, depth);
        else if (s->kind == STMT_IF) {
            n += loop_exits(s->thn, depth);
            n += loop_exits(s->els, depth);
        } else if (inner) {
            /* a goto inside still leaves this loop; a break does not */
            n += loop_exits(s->body, depth + 1) -
                 0;
        } else if (s->kind == STMT_LABEL) {
            n += loop_exits(s->body, depth);
        }
        if (s->kind == STMT_IF || s->kind == STMT_BLOCK || inner ||
            s->kind == STMT_LABEL)
            ;
    }
    return n;
}

/* the statements of a function, with its returns counted for 15.5 */
static int g_nret, g_ret_last;
static int g_ngoto_labels;
static const char *g_labels[256];
static int g_label_line[256], g_nlabels, g_label_used[256];

static void loop_float_counter(struct stmt *s)
{
    struct expr *st = s->step;
    if (st && (st->kind == EXPR_INCDEC || st->kind == EXPR_COMPOUND ||
               st->kind == EXPR_ASSIGN) && st->lhs && st->lhs->ty &&
        (st->lhs->ty->kind == TY_FLOAT || st->lhs->ty->kind == TY_DOUBLE ||
         st->lhs->ty->kind == TY_LDOUBLE)) {
        R("misra-14.1", st, "");
        R("cert-flp30", st, "");
    }
}

static void walk_stmt(struct stmt *s)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_EXPR:
            if (s->expr) {
                const struct expr *x = s->expr;
                if (!rules_side_effects(x) &&
                    !(x->kind == EXPR_CAST && x->ty && x->ty->kind == TY_VOID))
                    R("misra-2.2", x, "");
                if (x->kind == EXPR_CALL && x->ty && x->ty->kind != TY_VOID)
                    R("misra-17.7", x, callee_name(x));
                full_expr(s->expr, 0);
            }
            break;
        case STMT_DECL:
            if (s->dty) {
                int depth = 0;
                for (const struct type *t = s->dty; t && t->kind == TY_PTR;
                     t = t->pointee)
                    depth++;
                if (depth > 2)
                    R("misra-18.5", s, s->name);
                if (ty_is_vla(s->dty))
                    R("misra-18.8", s, s->name);
                if (s->dty->kind == TY_STRUCT && s->dty->is_union)
                    R("misra-19.2", s, s->name);
                if (s->name && !s->is_extern &&
                    (!strncmp(s->name, "__", 2) ||
                     (s->name[0] == '_' && s->name[1] >= 'A' && s->name[1] <= 'Z')))
                    R("cert-dcl37", s, s->name);
            }
            if (s->expr) {
                assigned(s->expr, s->dty, s->expr);
                full_expr(s->expr, 1);
            }
            for (int k = 0; s->inits && k < s->ninits; k++)
                full_expr(s->inits[k].e, 1);
            break;
        case STMT_RETURN:
            g_nret++;
            g_ret_last = !s->next;
            if (s->expr) {
                const struct expr *x = strip(s->expr);
                if (x && x->kind == EXPR_ADDR) {
                    const struct expr *b = x->rhs;
                    while (b && b->kind == EXPR_MEMBER && !b->is_arrow)
                        b = b->lhs;
                    if (b && b->kind == EXPR_VAR && b->var_index >= 0 &&
                        g_fn && b->var_index >= g_fn->nparams &&
                        b->var_index < g_fn->nvars)
                        R("cert-dcl30", s, b->name);
                }
                const struct type *dt = declared_type(x);
                if (x && x->kind == EXPR_VAR && dt && dt->kind == TY_ARRAY &&
                    g_fn && x->var_index >= g_fn->nparams &&
                    x->var_index < g_fn->nvars)
                    R("cert-dcl30", s, x->name);
                if (g_fn)
                    assigned(s->expr, g_fn->ret_ty, s->expr);
                full_expr(s->expr, 1);
            }
            break;
        case STMT_IF:
            cond_rules(s->cond, 0);
            full_expr(s->cond, 1);
            if (!is_compound(s->thn))
                R("misra-15.6", s->thn ? s->thn : s, "the if's body");
            if (s->els && !is_compound(s->els) && s->els->kind != STMT_IF)
                R("misra-15.6", s->els, "the else's body");
            if (s->els && s->els->kind == STMT_IF) {
                const struct stmt *k = s->els;
                while (k->els && k->els->kind == STMT_IF)
                    k = k->els;
                if (!k->els)
                    R("misra-15.7", k, "");
            }
            walk_stmt(s->thn);
            /* an else-if is its own statement, walked here */
            if (s->els) {
                struct stmt *e = s->els, *nx = e->next;
                e->next = NULL;
                walk_stmt(e);
                e->next = nx;
            }
            break;
        case STMT_WHILE: case STMT_FOR: case STMT_DO:
            if (s->cond)
                cond_rules(s->cond, 1);
            if (s->kind == STMT_FOR)
                loop_float_counter(s);
            if (!is_compound(s->body))
                R("misra-15.6", s->body ? s->body : s, "the loop's body");
            if (loop_exits(s->body && s->body->kind == STMT_BLOCK ? s->body->body
                                                                  : s->body, 0) > 1)
                R("misra-15.4", s, "");
            walk_stmt(s->initdecl);
            full_expr(s->init, 0);
            full_expr(s->cond, 1);
            full_expr(s->step, 0);
            walk_stmt(s->body);
            break;
        case STMT_SWITCH:
            switch_rules(s);
            if (!is_compound(s->body))
                R("misra-15.6", s->body ? s->body : s, "the switch's body");
            full_expr(s->cond, 1);
            walk_stmt(s->body);
            break;
        case STMT_BLOCK:
            walk_stmt(s->body);
            break;
        case STMT_GOTO:
            R("misra-15.1", s, "");
            if (s->name) {
                int found = 0;
                for (int k = 0; k < g_nlabels; k++)
                    if (!strcmp(g_labels[k], s->name)) {
                        found = 1;
                        g_label_used[k] = 1;
                        if (g_label_line[k] < s->line)
                            R("misra-15.2", s, s->name);
                    }
                (void)found;
            }
            full_expr(s->expr, 1);
            break;
        case STMT_LABEL:
            walk_stmt(s->body);
            break;
        default:
            walk_stmt(s->body);
            break;
        }
    }
}

/* labels first, so a goto knows which way it jumps */
static void collect_labels(const struct stmt *s)
{
    for (; s; s = s->next) {
        if (s->kind == STMT_LABEL && s->name && g_nlabels < 256) {
            g_labels[g_nlabels] = s->name;
            g_label_line[g_nlabels] = s->line;
            g_label_used[g_nlabels] = 0;
            g_nlabels++;
        }
        collect_labels(s->body);
        collect_labels(s->thn);
        collect_labels(s->els);
        collect_labels(s->initdecl);
    }
}

static void labels_by_address(const struct expr *e)
{
    if (!e)
        return;
    if (e->kind == EXPR_LABELADDR && e->name)
        for (int k = 0; k < g_nlabels; k++)
            if (!strcmp(g_labels[k], e->name))
                g_label_used[k] = 1;
    labels_by_address(e->lhs);
    labels_by_address(e->rhs);
    for (int k = 0; e->args && k < e->nargs; k++)
        labels_by_address(e->args[k]);
}

static void stmt_labels_by_address(const struct stmt *s)
{
    for (; s; s = s->next) {
        labels_by_address(s->expr);
        labels_by_address(s->cond);
        stmt_labels_by_address(s->body);
        stmt_labels_by_address(s->thn);
        stmt_labels_by_address(s->els);
    }
}

/* parameters never read (2.7), and MEM30's use after free in a block */
static int uses_var(const struct expr *e, int v)
{
    if (!e)
        return 0;
    if (e->kind == EXPR_VAR && e->var_index == v)
        return 1;
    if (uses_var(e->lhs, v) || uses_var(e->rhs, v))
        return 1;
    for (int k = 0; e->args && k < e->nargs; k++)
        if (uses_var(e->args[k], v))
            return 1;
    for (int k = 0; e->elems && k < e->nelems; k++)
        if (uses_var(e->elems[k], v))
            return 1;
    for (int k = 0; e->inits && k < e->ninits; k++)
        if (uses_var(e->inits[k].e, v))
            return 1;
    return 0;
}

static int stmt_uses_var(const struct stmt *s, int v)
{
    for (; s; s = s->next) {
        if (uses_var(s->expr, v) || uses_var(s->cond, v) ||
            uses_var(s->init, v) || uses_var(s->step, v) ||
            stmt_uses_var(s->body, v) || stmt_uses_var(s->thn, v) ||
            stmt_uses_var(s->els, v) || stmt_uses_var(s->initdecl, v))
            return 1;
        for (int k = 0; s->inits && k < s->ninits; k++)
            if (uses_var(s->inits[k].e, v))
                return 1;
    }
    return 0;
}

static void mem30_block(const struct stmt *s)
{
    for (; s; s = s->next) {
        if (s->kind == STMT_BLOCK) {
            mem30_block(s->body);
            continue;
        }
        if (s->kind == STMT_IF) { mem30_block(s->thn); mem30_block(s->els); }
        if (s->kind == STMT_WHILE || s->kind == STMT_FOR || s->kind == STMT_DO)
            mem30_block(s->body);
        const struct expr *x = s->kind == STMT_EXPR ? s->expr : NULL;
        if (!x || x->kind != EXPR_CALL || !callee_name(x) ||
            strcmp(callee_name(x), "free") || x->nargs < 1)
            continue;
        const struct expr *a = strip(x->args[0]);
        if (!a || a->kind != EXPR_VAR || a->var_index < 0)
            continue;
        int v = a->var_index;
        for (const struct stmt *k = s->next; k; k = k->next) {
            /* written again first: a new pointer */
            if (k->kind == STMT_EXPR && k->expr && k->expr->kind == EXPR_ASSIGN &&
                k->expr->lhs && k->expr->lhs->kind == EXPR_VAR &&
                k->expr->lhs->var_index == v)
                break;
            if (k->kind == STMT_RETURN && k->expr && strip(k->expr)->kind ==
                EXPR_VAR && strip(k->expr)->var_index == v)
                break;
            if (stmt_uses_var(k, v) && k->next != k) {
                const struct stmt one = *k;
                (void)one;
                R("cert-mem30", k->expr ? k->expr : x, a->name);
                break;
            }
        }
    }
}

/* ---- the unit -------------------------------------------------------- */

static int g_ncg;
static struct func **g_cg_from, **g_cg_to;

static void calls_of(const struct expr *e, struct func *f)
{
    if (!e)
        return;
    if (e->kind == EXPR_CALL && e->callee && e->callee->has_defn) {
        g_cg_from = xrealloc(g_cg_from, (size_t)(g_ncg + 1) * sizeof *g_cg_from);
        g_cg_to = xrealloc(g_cg_to, (size_t)(g_ncg + 1) * sizeof *g_cg_to);
        g_cg_from[g_ncg] = f;
        g_cg_to[g_ncg] = e->callee;
        g_ncg++;
    }
    calls_of(e->lhs, f);
    calls_of(e->rhs, f);
    for (int k = 0; e->args && k < e->nargs; k++)
        calls_of(e->args[k], f);
    for (int k = 0; e->inits && k < e->ninits; k++)
        calls_of(e->inits[k].e, f);
    if (e->kind == EXPR_STMTEXPR) {
        for (const struct stmt *s = e->body; s; s = s->next)
            calls_of(s->expr, f);
    }
}

static void stmt_calls(const struct stmt *s, struct func *f)
{
    for (; s; s = s->next) {
        calls_of(s->expr, f);
        calls_of(s->cond, f);
        calls_of(s->init, f);
        calls_of(s->step, f);
        for (int k = 0; s->inits && k < s->ninits; k++)
            calls_of(s->inits[k].e, f);
        stmt_calls(s->body, f);
        stmt_calls(s->thn, f);
        stmt_calls(s->els, f);
        stmt_calls(s->initdecl, f);
    }
}

/* does f reach itself through the unit's call graph? */
static int reaches(struct func *from, struct func *target, int depth,
                   struct func **seen, int *nseen)
{
    if (depth > 64)
        return 0;
    for (int k = 0; k < *nseen; k++)
        if (seen[k] == from)
            return 0;
    if (*nseen < 512)
        seen[(*nseen)++] = from;
    for (int k = 0; k < g_ncg; k++)
        if (g_cg_from[k] == from) {
            if (g_cg_to[k] == target)
                return 1;
            if (reaches(g_cg_to[k], target, depth + 1, seen, nseen))
                return 1;
        }
    return 0;
}

int rules_any(void)
{
    for (int i = 0; i < NRULES; i++)
        if (diag_warning_enabled(g_rules[i].name))
            return 1;
    return 0;
}

void rules_check(struct unit *u)
{
    g_any = rules_any();
    if (!g_any)
        return;
    g_unit = u;
    g_ncg = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->body)
            stmt_calls(f->body, f);
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->ty)
            continue;
        g_file = g->file ? g->file : u->file;
        struct expr at;
        memset(&at, 0, sizeof at);
        at.line = g->name_line ? g->name_line : g->line;
        at.col = g->name_line ? g->name_col : 0;
        if (g->is_extern && g->ty->kind == TY_ARRAY && g->ty->count <= 0)
            R("misra-8.11", &at, g->name);
        int depth = 0;
        for (const struct type *t = g->ty; t && t->kind == TY_PTR; t = t->pointee)
            depth++;
        if (depth > 2)
            R("misra-18.5", &at, g->name);
        if (g->ty->kind == TY_STRUCT && g->ty->is_union)
            R("misra-19.2", &at, g->name);
        if (g->ty->kind == TY_STRUCT && g->ty->nmembers > 0) {
            const struct member *m = &g->ty->members[g->ty->nmembers - 1];
            if (m->ty && m->ty->kind == TY_ARRAY && m->ty->count <= 0)
                R("misra-18.7", &at, g->name);
        }
        if (g->name && (!strncmp(g->name, "__", 2) ||
                        (g->name[0] == '_' && g->name[1] >= 'A' &&
                         g->name[1] <= 'Z')) && !g->is_extern)
            R("cert-dcl37", &at, g->name);
    }
    for (struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed || !f->body)
            continue;
        g_fn = f;
        g_file = f->file ? f->file : u->file;
        struct expr at;
        memset(&at, 0, sizeof at);
        at.line = f->name_line ? f->name_line : f->line;
        at.col = f->name_line ? f->name_col : 0;
        if (f->def_inline && !f->is_static)
            R("misra-8.10", &at, f->name);
        if (f->name && (!strncmp(f->name, "__", 2) ||
                        (f->name[0] == '_' && f->name[1] >= 'A' &&
                         f->name[1] <= 'Z')))
            R("cert-dcl37", &at, f->name);
        {
            struct func *seen[512];
            int nseen = 0;
            if (reaches(f, f, 0, seen, &nseen))
                R("misra-17.2", &at, f->name);
        }
        for (int k = 0; k < f->nparams; k++)
            if (f->params[k] && !stmt_uses_var(f->body, k))
                R("misra-2.7", &at, f->params[k]);
        g_nret = 0;
        g_ret_last = 0;
        g_nlabels = 0;
        collect_labels(f->body);
        stmt_labels_by_address(f->body);
        walk_stmt(f->body);
        mem30_block(f->body);
        for (int k = 0; k < g_nlabels; k++)
            if (!g_label_used[k]) {
                struct expr l;
                memset(&l, 0, sizeof l);
                l.line = g_label_line[k];
                R("misra-2.6", &l, g_labels[k]);
            }
        /* 15.5: one return, the function's last statement */
        const struct stmt *last = NULL;
        for (const struct stmt *s = f->body->kind == STMT_BLOCK ? f->body->body
                                                                : f->body;
             s; s = s->next)
            last = s;
        if (g_nret > 1 || (g_nret == 1 && (!last || last->kind != STMT_RETURN)))
            R("misra-15.5", &at, f->name);
        (void)g_ngoto_labels;
    }
    g_fn = NULL;
    free(g_cg_from);
    free(g_cg_to);
    g_cg_from = g_cg_to = NULL;
    g_ncg = 0;
}
