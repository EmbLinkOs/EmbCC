/* -Wuninitialized / -Wmaybe-uninitialized (docs/manual/diagnostics.md T4).
 *
 * `int x; if (c) x = 1; return x;` compiles, links and runs, and returns
 * whatever the frame happened to hold. No diagnostic so far in EmbCC said
 * so, because saying so is not a check on one expression: it is a question
 * about every path that reaches the read.
 *
 * So this is a real dataflow analysis, run over the statement tree after
 * semantic analysis has resolved every name to a frame slot. Each slot
 * holds one of three values — written on no path, on every path, on some —
 * and the walk joins them wherever control flow rejoins: after an `if`,
 * around a loop, at a `case` label. A read of a slot that is written on no
 * path is `-Wuninitialized`; on some, `-Wmaybe-uninitialized`.
 *
 * Where it cannot see, it says nothing rather than guessing. A variable
 * whose address is taken can be written through that pointer by anything;
 * `goto` can carry any state to a label; an `asm` block writes what its
 * constraints say. Each of those turns the analysis off for what it
 * touches. A false "may be used uninitialized" costs the reader more than
 * a missed one, because the missed one is still a bug they can find and
 * the false one is a bug they cannot.
 *
 * -Wnull-dereference is the same walk asking a different question of each
 * pointer slot (`nullmode`): is it null here -- on every path (NL_NULL),
 * on some (NL_MAYBE), on none that we know of (NL_NON: it was pointed at
 * something, or dereferenced already), or unknown (NL_UNK: a parameter, a
 * call's result). Assignments set it from what they store; `if (p)`,
 * `p == 0`, `p && ...` and a loop's condition split it by outcome. A
 * dereference of a slot that is NL_NULL is "null pointer dereference", of
 * one that is NL_MAYBE "potential null pointer dereference", as GCC words
 * them -- GCC finds them after optimizing, and only where address 0 is no
 * object; this finds them on every target, at every level, but never in a
 * volatile access, which is how a Cortex-M reads its vector table at 0.
 */
#include "uninit.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "sema.h"
#include "type.h"

/* What a slot holds at a program point. */
enum { U_NO, U_YES, U_MAYBE };
/* ...and in nullmode, what a pointer slot is */
enum { NL_UNK, NL_NULL, NL_NON, NL_MAYBE };

struct state {
    unsigned char *v;
    int reach;            /* 0: control cannot get here (after a return) */
};

/* A loop or switch, so `break` and `continue` know where they land. */
struct target {
    struct state brk;
    struct state cont;
    int has_cont;
    struct target *up;
};

struct ctx {
    const char *file;
    const struct uninit_var *vars;
    int nvars;
    unsigned char *track;     /* this slot is one we can reason about */
    unsigned char *warned;    /* already reported: one warning per variable */
    struct target *targets;
    unsigned char *backedge;  /* slots a loop's back edge alone made MAYBE */
    int cond_depth;           /* inside an if/case arm */
    int quiet;                /* a pre-pass: compute, do not report */
    int nullmode;             /* -Wnull-dereference's question, not ours */
    struct func *f;
};

static struct state st_new(struct ctx *c, int reach)
{
    struct state s;
    s.v = xmalloc((size_t)(c->nvars ? c->nvars : 1));
    memset(s.v, U_NO, (size_t)c->nvars);
    s.reach = reach;
    return s;
}

static struct state st_copy(struct ctx *c, const struct state *a)
{
    struct state s = st_new(c, a->reach);
    memcpy(s.v, a->v, (size_t)c->nvars);
    return s;
}

static void st_free(struct state *s) { free(s->v); s->v = NULL; }

/* Two paths meet: a slot both agree on keeps its value, one they differ
 * on becomes "on some paths". An unreachable side contributes nothing. */
static void st_join(struct ctx *c, struct state *a, const struct state *b)
{
    if (!b->reach)
        return;
    if (!a->reach) {
        memcpy(a->v, b->v, (size_t)c->nvars);
        a->reach = 1;
        return;
    }
    for (int i = 0; i < c->nvars; i++)
        if (a->v[i] != b->v[i]) {
            if (!c->nullmode)
                a->v[i] = U_MAYBE;
            else            /* null on one side: null on some paths */
                a->v[i] = a->v[i] == NL_NULL || a->v[i] == NL_MAYBE ||
                          b->v[i] == NL_NULL || b->v[i] == NL_MAYBE
                          ? NL_MAYBE : NL_UNK;
        }
}

static int st_same(struct ctx *c, const struct state *a, const struct state *b)
{
    return a->reach == b->reach && !memcmp(a->v, b->v, (size_t)c->nvars);
}

/* ---- what we can and cannot reason about ---- */

/* Only scalars. An aggregate is partly written a member at a time, and
 * "was the whole thing written" is a different, much weaker question. */
static int scalar(struct type *t)
{
    if (!t)
        return 0;
    switch (t->kind) {
    case TY_STRUCT: case TY_ARRAY: case TY_VOID: case TY_FUNC:
        return 0;
    default:
        return !ty_is_vla(t);
    }
}

/* A name the compiler invented for its own lowering is not the
 * programmer's to initialize. */
static int ours(const char *n)
{
    return !n || n[0] == '<' || !strcmp(n, "this") || !strncmp(n, "__cx_", 5);
}

/* The pre-pass: anything that would make the walk lie. `&x` hands the slot
 * to code we are not looking at; a `goto` can reach a label with any state
 * at all; an `asm` writes whatever its constraints name; an exception
 * region is C++'s lowering, whose edges are not in this tree. */
static void scan_expr(struct ctx *c, struct expr *e, int *off);
static void scan_stmt(struct ctx *c, struct stmt *s, int *off);

static void untrack(struct ctx *c, struct expr *e)
{
    while (e && (e->kind == EXPR_CAST || e->kind == EXPR_MEMBER))
        e = e->kind == EXPR_MEMBER ? e->lhs : e->rhs;
    if (e && e->kind == EXPR_VAR && !e->gref && e->var_index >= 0 &&
        e->var_index < c->nvars)
        c->track[e->var_index] = 0;
}

static void scan_expr(struct ctx *c, struct expr *e, int *off)
{
    if (!e || *off)
        return;
    if (e->kind == EXPR_ADDR)
        untrack(c, e->rhs);
    if (e->kind == EXPR_LABELADDR) {
        *off = 1;                       /* &&label: a computed goto follows */
        return;
    }
    scan_expr(c, e->lhs, off);
    scan_expr(c, e->rhs, off);
    for (int i = 0; i < e->nargs; i++)
        scan_expr(c, e->args[i], off);
    for (int i = 0; i < e->nelems; i++)
        scan_expr(c, e->elems[i], off);
    for (int i = 0; i < e->ninits; i++)
        scan_expr(c, e->inits[i].e, off);
    scan_stmt(c, e->body, off);
}

static void scan_stmt(struct ctx *c, struct stmt *s, int *off)
{
    for (; s && !*off; s = s->next) {
        if (s->kind == STMT_GOTO || s->kind == STMT_LABEL ||
            s->kind == STMT_EHREGION) {
            *off = 1;
            return;
        }
        if (s->kind == STMT_ASM) {
            /* Every operand's lvalue is beyond us, output or not. */
            if (s->asm_s) {
                for (int i = 0; i < s->asm_s->nout; i++)
                    untrack(c, s->asm_s->out[i].expr);
                for (int i = 0; i < s->asm_s->nin; i++)
                    untrack(c, s->asm_s->in[i].expr);
            }
        }
        scan_expr(c, s->expr, off);
        scan_expr(c, s->cond, off);
        scan_expr(c, s->init, off);
        scan_expr(c, s->step, off);
        for (int i = 0; i < s->ninits; i++)
            scan_expr(c, s->inits[i].e, off);
        scan_stmt(c, s->initdecl, off);
        scan_stmt(c, s->thn, off);
        scan_stmt(c, s->els, off);
        scan_stmt(c, s->body, off);
    }
}

/* ---- the walk ---- */

static void ex(struct ctx *c, struct state *st, struct expr *e);
static void chain(struct ctx *c, struct state *st, struct expr *e);
static void stm(struct ctx *c, struct state *st, struct stmt *s);

/* One warning per variable: the first read that can see it uninitialized
 * is the one to fix, and the rest are the same bug seen again. */
static void report(struct ctx *c, struct expr *e, int slot, int maybe)
{
    if (c->quiet || c->warned[slot])
        return;
    /* A value the loop carries -- written late in the body, read early on
     * a later turn -- is MAYBE at the top only because of the back edge.
     * Read under a test inside that loop, the test is almost always the
     * guard that makes it safe (`if (i) use(w); w = i;`), and the analysis
     * cannot relate the test to the iteration count. gcc does not warn
     * here either. Unguarded, it is a real first-iteration read and is
     * still reported. */
    if (maybe && c->cond_depth > 0 && c->backedge && c->backedge[slot])
        return;
    c->warned[slot] = 1;
    const struct uninit_var *v = &c->vars[slot];
    if (c->nullmode) {
        diag_warn_opt(c->file, e->line, e->col, "null-dereference",
                      maybe ? "potential null pointer dereference: '%s' is "
                              "null on some path to here"
                            : "null pointer dereference: '%s' is null here",
                      v->name);
        return;
    }
    diag_warn_opt(c->file, e->line, e->col,
                  maybe ? "maybe-uninitialized" : "uninitialized",
                  maybe ? "'%s' may be used uninitialized"
                        : "'%s' is used uninitialized", v->name);
    if (v->line)
        diag_note_at(c->file, v->line, v->col,
                     "'%s' is declared here, with no initializer", v->name);
}

/* The slot an expression names, or -1 if it does not name a tracked one. */
static int slot_of(struct ctx *c, struct expr *e)
{
    if (!e || e->kind != EXPR_VAR)
        if (!e || e->kind != EXPR_INCDEC)
            return -1;
    if (e->gref || e->fref)               /* a global, or a function\'s name */
        return -1;
    if (e->var_index < 0 || e->var_index >= c->nvars)
        return -1;
    /* var_index is only meaningful once sema has bound the name to a frame
     * slot; a name it bound to anything else keeps the field\'s default,
     * which is slot 0 -- a real slot belonging to some other variable. The
     * name is what says the binding happened. */
    if (!e->name || !c->vars[e->var_index].name ||
        strcmp(e->name, c->vars[e->var_index].name) != 0)
        return -1;
    return c->track[e->var_index] ? e->var_index : -1;
}

static void read_var(struct ctx *c, struct state *st, struct expr *e)
{
    int s = slot_of(c, e);
    if (s < 0 || !st->reach || c->nullmode)
        return;
    if (st->v[s] == U_NO)
        report(c, e, s, 0);
    else if (st->v[s] == U_MAYBE)
        report(c, e, s, 1);
}

static void write_var(struct ctx *c, struct state *st, struct expr *e)
{
    int s = slot_of(c, e);
    if (s >= 0)
        st->v[s] = c->nullmode ? NL_UNK : U_YES;
}

static void refine(struct ctx *c, struct state *st, struct expr *e, int truth);

/* nullmode: what storing e makes a pointer -- a null pointer constant
 * (`0`, `(void *)0`, NULL), an address (`&x`, a string, an array or a
 * function's name), another tracked pointer's state, or unknown */
static int null_of(struct ctx *c, struct state *st, struct expr *e)
{
    while (e && (e->kind == EXPR_CAST || e->kind == EXPR_COMMA ||
                 e->kind == EXPR_ASSIGN))
        e = e->rhs;
    if (!e)
        return NL_UNK;
    if (e->kind == EXPR_NUM)
        return e->num == 0 ? NL_NULL : NL_UNK;
    if (e->kind == EXPR_ADDR || e->kind == EXPR_STR)
        return NL_NON;
    if (e->kind == EXPR_COND) {         /* `c ? q : 0`: as its arms join */
        /* each arm as its outcome of the condition left it: in
         * `v ? v : m->body`, v is not null where it is chosen */
        struct state t = st_copy(c, st), f = st_copy(c, st);
        refine(c, &t, e->args[0], 1);
        refine(c, &f, e->args[0], 0);
        int a = null_of(c, &t, e->lhs), b = null_of(c, &f, e->rhs);
        st_free(&t);
        st_free(&f);
        if (a == b)
            return a;
        return a == NL_NULL || a == NL_MAYBE || b == NL_NULL ||
               b == NL_MAYBE ? NL_MAYBE : NL_UNK;
    }
    if (e->kind == EXPR_VAR) {
        if (e->fref || (e->ty && (e->ty->kind == TY_ARRAY ||
                                  e->ty->kind == TY_FUNC)))
            return NL_NON;
        int s = slot_of(c, e);
        return s >= 0 ? st->v[s] : NL_UNK;
    }
    return NL_UNK;
}

/* nullmode: a decision -- an `if`, a loop's test, an operand of `&&` --
 * may be correlated with how a pointer came to be null on some paths:
 * `nx = n < k ? &a[n] : 0; int fuse = nx && ...; if (fuse) use(nx->x)`
 * never reads it null, and only the condition knows. So "null on some
 * path" lasts until the next decision; a test of the pointer itself
 * (refine) still says what it is, and "null on every path" stays. Over
 * EmbCC's own sources the analysis without this reported 158 such reads,
 * every one of them guarded. */
static void forget(struct ctx *c, struct state *st)
{
    if (!c->nullmode)
        return;
    for (int i = 0; i < c->nvars; i++)
        if (st->v[i] == NL_MAYBE)
            st->v[i] = NL_UNK;
}

/* nullmode: the state where condition e had the outcome `truth` -- for a
 * pointer tested bare (`if (p)`, `!p`) or against a null constant */
static void refine(struct ctx *c, struct state *st, struct expr *e, int truth)
{
    if (!c->nullmode || !st->reach)
        return;
    while (e && e->kind == EXPR_CAST)
        e = e->rhs;
    if (e && e->kind == EXPR_NOT) {
        refine(c, st, e->rhs, !truth);
        return;
    }
    if (e && e->kind == EXPR_BINOP && (e->op == B_EQ || e->op == B_NE)) {
        struct expr *v = e->lhs, *k = e->rhs;
        while (v && v->kind == EXPR_CAST)
            v = v->rhs;
        while (k && k->kind == EXPR_CAST)
            k = k->rhs;
        if (v && v->kind == EXPR_NUM) {       /* 0 == p */
            struct expr *t = v;
            v = k;
            k = t;
        }
        if (!k || k->kind != EXPR_NUM || k->num != 0)
            return;
        int s = slot_of(c, v);
        if (s >= 0)          /* p == 0 true, or p != 0 false: null */
            st->v[s] = (e->op == B_EQ) == !!truth ? NL_NULL : NL_NON;
        return;
    }
    int s = slot_of(c, e);
    if (s >= 0)
        st->v[s] = truth ? NL_NON : NL_NULL;
}

/* nullmode: a dereference of the pointer e (the operand of `*`, the base
 * of `->`, `p + i` under either): reported if it is null here; after it,
 * the pointer is not -- it was used, so it pointed at something. */
static void deref(struct ctx *c, struct state *st, struct expr *e, int vol)
{
    while (e && e->kind == EXPR_CAST)
        e = e->rhs;
    if (e && e->kind == EXPR_BINOP && (e->op == B_ADD || e->op == B_SUB)) {
        struct expr *l = e->lhs;
        while (l && l->kind == EXPR_CAST)
            l = l->rhs;
        e = l && l->ty && l->ty->kind == TY_PTR ? l : e->rhs;
        while (e && e->kind == EXPR_CAST)
            e = e->rhs;
    }
    int s = slot_of(c, e);
    if (s < 0 || !st->reach)
        return;
    if (!vol && st->v[s] == NL_NULL)
        report(c, e, s, 0);
    else if (!vol && st->v[s] == NL_MAYBE)
        report(c, e, s, 1);
    st->v[s] = NL_NON;
}

/* `a && b && c` short-circuits, but not independently: c runs only if b
 * ran and was true. Taking the operands pairwise loses that -- c would be
 * analysed from the state where b MIGHT not have run, and a variable b
 * assigns and c uses would look like it might be uninitialized. So the
 * whole chain of one operator is walked at once: each operand sees the
 * state in which every operand before it ran, and only the RESULT joins
 * the points the chain could have stopped at.
 */
static void chain(struct ctx *c, struct state *st, struct expr *e)
{
    struct expr *ops[64];
    int n = 0;
    enum binop op = e->op;
    for (struct expr *p = e; ; ) {      /* the left spine, same operator */
        if (p->kind == EXPR_BINOP && p->op == op && n < 63) {
            ops[n++] = p->rhs;
            p = p->lhs;
            continue;
        }
        ops[n++] = p;
        break;
    }
    struct state any = st_new(c, 0);
    ex(c, st, ops[n - 1]);              /* the leftmost operand always runs */
    c->cond_depth++;
    for (int i = n - 2; i >= 0; i--) {
        st_join(c, &any, st);           /* the chain could have stopped here */
        /* the next operand runs where this one was true (&&) or false */
        forget(c, st);
        refine(c, st, ops[i + 1], op == B_LAND);
        ex(c, st, ops[i]);
    }
    c->cond_depth--;
    st_join(c, st, &any);
    st_free(&any);
}

/* A condition, split by its outcome: *t the state where it was true, *f
 * where it was false. `if (fseek(f, 0, 2) || (n = ftell(f)) < 0) return;`
 * leaves n written for what follows, because the else side -- the
 * condition false -- is where both operands ran; walked as one value the
 * condition left n written on SOME paths, and `use(n)` after it was a
 * false "may be used uninitialized". So: `a && b` is true where b was,
 * from where a was true, and false where either was; `a || b` the other
 * way round; `!a` swaps them; anything else is one state for both. */
static void cond2(struct ctx *c, struct state *st, struct expr *e,
                  struct state *t, struct state *f)
{
    while (e && e->kind == EXPR_CAST && e->rhs && scalar(e->ty))
        e = e->rhs;                     /* (int)(a || b) splits the same */
    if (e && e->kind == EXPR_NOT) {
        cond2(c, st, e->rhs, f, t);
        return;
    }
    if (e && e->kind == EXPR_BINOP && (e->op == B_LAND || e->op == B_LOR)) {
        int and = e->op == B_LAND;
        struct state ta, fa, tb, fb;
        cond2(c, st, e->lhs, &ta, &fa);
        c->cond_depth++;                /* the right runs on one side only */
        cond2(c, and ? &ta : &fa, e->rhs, &tb, &fb);
        c->cond_depth--;
        if (and) {
            *t = tb;
            st_join(c, &fa, &fb);
            *f = fa;
            st_free(&ta);
            st_free(&fb);
        } else {
            st_join(c, &ta, &tb);
            *t = ta;
            *f = fb;
            st_free(&fa);
            st_free(&tb);
        }
        return;
    }
    ex(c, st, e);
    forget(c, st);
    *t = st_copy(c, st);
    *f = st_copy(c, st);
    refine(c, t, e, 1);
    refine(c, f, e, 0);
}

static void ex(struct ctx *c, struct state *st, struct expr *e)
{
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_VAR:
        read_var(c, st, e);
        return;
    case EXPR_SIZEOF: case EXPR_ALIGNOF:
        return;                         /* unevaluated */
    case EXPR_CAST:
        /* `(void)x` is how C says "unused on purpose", and GCC and clang
         * take it so: it reads nothing worth a -Wuninitialized. Anything
         * more than a bare variable is still walked. */
        if (e->cast_ty && e->cast_ty->kind == TY_VOID && e->rhs &&
            e->rhs->kind == EXPR_VAR)
            return;
        break;
    case EXPR_ADDR:
        /* &p->m and &a[i] compute an address: nothing is read through
         * the pointer (offsetof's idiom is `&((T *)0)->m`) */
        if (c->nullmode) {
            struct expr *r = e->rhs;
            while (r && (r->kind == EXPR_MEMBER || r->kind == EXPR_DEREF))
                r = r->kind == EXPR_MEMBER ? r->lhs : r->rhs;
            if (r && r != e->rhs)
                ex(c, st, r);
        }
        return;                         /* the slot is untracked already */
    case EXPR_DEREF:
        if (c->nullmode) {
            ex(c, st, e->rhs);
            deref(c, st, e->rhs, e->ty && e->ty->is_volatile);
            return;
        }
        break;
    case EXPR_MEMBER:
        if (c->nullmode && e->is_arrow) {
            ex(c, st, e->lhs);
            deref(c, st, e->lhs, e->lhs && e->lhs->ty &&
                  e->lhs->ty->pointee && e->lhs->ty->pointee->is_volatile);
            return;
        }
        break;
    case EXPR_ASSIGN:
        ex(c, st, e->rhs);
        if (slot_of(c, e->lhs) >= 0) {
            write_var(c, st, e->lhs);
            if (c->nullmode)
                st->v[slot_of(c, e->lhs)] = (unsigned char)null_of(c, st, e->rhs);
        } else {
            ex(c, st, e->lhs);          /* *p = , a.b = : the base is read */
        }
        return;
    case EXPR_COMPOUND:                 /* x op= y reads x, then writes it */
        ex(c, st, e->lhs);
        ex(c, st, e->rhs);
        write_var(c, st, e->lhs);
        return;
    case EXPR_INCDEC:
        if (slot_of(c, e) >= 0) {
            read_var(c, st, e);
            write_var(c, st, e);
        } else {
            ex(c, st, e->lhs);
            ex(c, st, e->rhs);
        }
        return;
    case EXPR_BINOP:
        if (e->op == B_LAND || e->op == B_LOR) {
            chain(c, st, e);
            return;
        }
        ex(c, st, e->lhs);
        ex(c, st, e->rhs);
        return;
    case EXPR_CALL:
        if (is_noreturn_call(e)) {      /* exit(), abort(), a panic() */
            for (int i = 0; i < e->nargs; i++)
                ex(c, st, e->args[i]);
            st->reach = 0;              /* nothing after this runs */
            return;
        }
        /* `va_start(ap, last)` READS nothing and WRITES ap -- irgen takes
         * its address. Spelled as a call, it looks like a read of an
         * uninitialized va_list, which is exactly backwards. */
        if (e->name && (!strcmp(e->name, "__builtin_va_start") ||
                        !strcmp(e->name, "__builtin_va_copy"))) {
            for (int i = 1; i < e->nargs; i++)
                ex(c, st, e->args[i]);
            write_var(c, st, e->args[0]);
            return;
        }
        break;
    case EXPR_COND: {
        struct state tt, ff;
        cond2(c, st, e->args[0], &tt, &ff);
        c->cond_depth++;
        ex(c, &tt, e->lhs);
        ex(c, &ff, e->rhs);
        c->cond_depth--;
        st_join(c, &tt, &ff);
        memcpy(st->v, tt.v, (size_t)c->nvars);
        st->reach = tt.reach;
        st_free(&tt);
        st_free(&ff);
        return;
    }
    default:
        break;
    }
    ex(c, st, e->lhs);
    ex(c, st, e->rhs);
    for (int i = 0; i < e->nargs; i++)
        ex(c, st, e->args[i]);
    for (int i = 0; i < e->nelems; i++)
        ex(c, st, e->elems[i]);
    for (int i = 0; i < e->ninits; i++)
        ex(c, st, e->inits[i].e);
    if (e->body)
        stm(c, st, e->body);
}

/* A loop that leaves only by `break`: `for (;;)`, `while (1)`. */
static int always_true(const struct expr *cond)
{
    while (cond && cond->kind == EXPR_CAST)
        cond = cond->rhs;
    return !cond || (cond->kind == EXPR_NUM && cond->num != 0);
}

/* A loop body, run to a fixed point: once with warnings suppressed so that
 * what the body writes is known at its own top, then once for real. Three
 * values deep, the lattice settles in that one extra pass. A do-while
 * (at_least_once) tests its condition here, after the body: the test sees
 * what the body wrote, and a `break` skips it. */
static void loop_body(struct ctx *c, struct state *st, struct stmt *body,
                      struct expr *step, struct expr *cond, int at_least_once)
{
    struct state entry = st_copy(c, st);
    unsigned char *saved_be = c->backedge;
    struct target t;
    t.brk = st_new(c, 0);
    t.cont = st_new(c, 0);
    t.has_cont = 1;
    t.up = c->targets;

    for (int pass = 0; pass < 2; pass++) {
        c->targets = &t;
        if (pass == 0)
            c->quiet++;
        struct state cur = st_copy(c, st);
        forget(c, &cur);                /* the loop's test is a decision */
        if (!at_least_once && cond)
            refine(c, &cur, cond, 1);   /* the body runs where it held */
        stm(c, &cur, body);
        st_join(c, &cur, &t.cont);
        ex(c, &cur, step);
        if (at_least_once)
            ex(c, &cur, cond);
        if (pass == 0) {
            c->quiet--;
            /* what the top of the loop can see on a later iteration */
            struct state top = st_copy(c, &entry);
            st_join(c, &top, &cur);
            if (!at_least_once)
                ex(c, &top, cond);
            /* Which slots the back edge alone raised to MAYBE. */
            c->backedge = xmalloc((size_t)(c->nvars ? c->nvars : 1));
            for (int k = 0; k < c->nvars; k++)
                c->backedge[k] = (unsigned char)
                    ((saved_be && saved_be[k]) ||
                     (entry.v[k] == U_NO && top.v[k] == U_MAYBE));
            if (!st_same(c, &top, st)) {
                st_free(st);
                *st = top;              /* what the loop top really sees */
            } else {
                st_free(&top);          /* already at the fixed point */
            }
            /* the reporting pass starts from empty accumulators */
            memset(t.brk.v, U_NO, (size_t)c->nvars); t.brk.reach = 0;
            memset(t.cont.v, U_NO, (size_t)c->nvars); t.cont.reach = 0;
            st_free(&cur);
            continue;
        }
        st_free(st);
        *st = cur;
        c->targets = t.up;
    }
    c->targets = t.up;

    /* Leaving: either the body ran and fell out, or a break did, or -- for
     * a loop whose condition is tested first -- it never ran at all. A
     * loop whose condition is always true leaves only by a break. */
    if (always_true(cond))
        st->reach = 0;
    st_join(c, st, &t.brk);
    if (!at_least_once && !always_true(cond))
        st_join(c, st, &entry);
    st_free(&entry);
    free(c->backedge);
    c->backedge = saved_be;
    st_free(&t.brk);
    st_free(&t.cont);
}

static void stm(struct ctx *c, struct state *st, struct stmt *s)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_DECL:
            if (s->var_index >= 0 && s->var_index < c->nvars &&
                c->track[s->var_index] && !c->nullmode) {
                if (s->expr || s->ninits)
                    st->v[s->var_index] = U_YES;
                else
                    st->v[s->var_index] = U_NO;
            }
            ex(c, st, s->expr);
            for (int i = 0; i < s->ninits; i++)
                ex(c, st, s->inits[i].e);
            if (c->nullmode && s->var_index >= 0 &&
                s->var_index < c->nvars && c->track[s->var_index])
                st->v[s->var_index] = (unsigned char)
                    (s->expr ? null_of(c, st, s->expr) : NL_UNK);
            break;
        case STMT_EXPR:
            ex(c, st, s->expr);
            break;
        case STMT_RETURN:
            ex(c, st, s->expr);
            st->reach = 0;
            break;
        case STMT_IF: {
            struct state tt, ff;
            cond2(c, st, s->cond, &tt, &ff);
            c->cond_depth++;
            stm(c, &tt, s->thn);
            stm(c, &ff, s->els);
            c->cond_depth--;
            st_join(c, &tt, &ff);
            memcpy(st->v, tt.v, (size_t)c->nvars);
            st->reach = tt.reach;
            st_free(&tt);
            st_free(&ff);
            break;
        }
        case STMT_WHILE:
            ex(c, st, s->cond);
            loop_body(c, st, s->body, NULL, s->cond, 0);
            break;
        case STMT_DO:
            loop_body(c, st, s->body, NULL, s->cond, 1);
            break;
        case STMT_FOR:
            stm(c, st, s->initdecl);
            ex(c, st, s->init);
            ex(c, st, s->cond);
            loop_body(c, st, s->body, s->step, s->cond, 0);
            break;
        case STMT_SWITCH: {
            ex(c, st, s->cond);
            forget(c, st);
            struct state head = st_copy(c, st);
            struct target t;
            t.brk = st_new(c, 0);
            t.cont = st_new(c, 0);
            t.has_cont = 0;
            t.up = c->targets;
            c->targets = &t;
            /* Case labels are markers in one list, not nested nodes: each
             * is another way in, carrying the state at the switch head. */
            int saw_default = 0;
            struct state cur = st_copy(c, &head);
            for (struct stmt *b = s->body && s->body->kind == STMT_BLOCK
                                  ? s->body->body : s->body;
                 b; b = b->next) {
                if (b->kind == STMT_CASE || b->kind == STMT_DEFAULT) {
                    st_join(c, &cur, &head);
                    if (b->kind == STMT_DEFAULT)
                        saw_default = 1;
                    continue;
                }
                struct stmt one = *b;
                one.next = NULL;
                c->cond_depth++;
                stm(c, &cur, &one);
                c->cond_depth--;
            }
            c->targets = t.up;
            st_join(c, &cur, &t.brk);
            if (!saw_default)
                st_join(c, &cur, &head);   /* no case matched */
            st_free(st);
            *st = cur;
            st_free(&head);
            st_free(&t.brk);
            st_free(&t.cont);
            break;
        }
        case STMT_BREAK:
            if (c->targets)
                st_join(c, &c->targets->brk, st);
            st->reach = 0;
            break;
        case STMT_CONTINUE:
            if (c->targets && c->targets->has_cont)
                st_join(c, &c->targets->cont, st);
            st->reach = 0;
            break;
        case STMT_BLOCK:
            stm(c, st, s->body);
            break;
        case STMT_CASE: case STMT_DEFAULT:
            break;                      /* handled by STMT_SWITCH */
        default:
            ex(c, st, s->expr);
            ex(c, st, s->cond);
            stm(c, st, s->body);
            break;
        }
    }
}

static void null_check(const char *file, struct func *f,
                       const struct uninit_var *vars, int nvars);

void uninit_check(const char *file, struct func *f,
                  const struct uninit_var *vars, int nvars)
{
    if (!f->body || nvars <= 0)
        return;
    if (diag_warning_enabled("null-dereference"))
        null_check(file, f, vars, nvars);
    if (!diag_warning_enabled("uninitialized") &&
        !diag_warning_enabled("maybe-uninitialized"))
        return;

    struct ctx c;
    memset(&c, 0, sizeof c);
    c.file = file;
    c.vars = vars;
    c.nvars = nvars;
    c.f = f;
    c.track = xmalloc((size_t)nvars);
    c.warned = xmalloc((size_t)nvars);
    memset(c.warned, 0, (size_t)nvars);
    for (int i = 0; i < nvars; i++)
        c.track[i] = (unsigned char)(!vars[i].is_param && !vars[i].is_static &&
                                     !ours(vars[i].name) && scalar(vars[i].ty));

    int off = 0;
    scan_stmt(&c, f->body, &off);
    if (off) {                          /* goto, a label, or C++'s lowering */
        free(c.track);
        free(c.warned);
        return;
    }

    struct state st = st_new(&c, 1);
    for (int i = 0; i < nvars; i++)
        if (!c.track[i])
            st.v[i] = U_YES;
    stm(&c, &st, f->body);
    st_free(&st);
    free(c.track);
    free(c.warned);
}

/* -Wnull-dereference: the same walk in nullmode, over every pointer slot
 * -- parameters too, which arrive unknown -- that nothing takes the
 * address of. */
static void null_check(const char *file, struct func *f,
                       const struct uninit_var *vars, int nvars)
{
    struct ctx c;
    memset(&c, 0, sizeof c);
    c.file = file;
    c.vars = vars;
    c.nvars = nvars;
    c.f = f;
    c.nullmode = 1;
    c.track = xmalloc((size_t)nvars);
    c.warned = xcalloc((size_t)nvars, 1);
    for (int i = 0; i < nvars; i++)
        c.track[i] = (unsigned char)(!vars[i].is_static && !ours(vars[i].name) &&
                                     vars[i].ty && vars[i].ty->kind == TY_PTR);
    int off = 0;
    scan_stmt(&c, f->body, &off);
    if (!off) {
        struct state st = st_new(&c, 1);   /* NL_UNK everywhere */
        stm(&c, &st, f->body);
        st_free(&st);
    }
    free(c.track);
    free(c.warned);
}
