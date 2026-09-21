/* C++ access control — [class.access].
 *
 * `private` and `protected` were parsed and recorded for years and
 * never enforced: reading another class's private member compiled, and
 * so did casting to a private base. This is the enforcement, and it is
 * a different kind of change from everything else in this front end.
 * Every other feature makes a valid program work; this one can only
 * make a program that compiles today stop compiling. It cannot fix a
 * single bug in a working program.
 *
 * Which is why the bias here runs one way. Where the rule is clear it
 * is applied; where this compiler cannot see enough to be sure, access
 * is GRANTED rather than refused. A missed diagnostic is a feature not
 * yet complete. A refusal of correct code is a compiler nobody can use,
 * and the corpus it would refuse — libstdc++, this project's own 97
 * headers, EmbLinkOS — is the regression test.
 *
 * ---- what "the context" means -------------------------------------------
 *
 * Every check is relative to the class whose member or friend is doing
 * the naming. Inside a member function of C, C's own privates are
 * reachable; outside, they are not. So the context is the innermost
 * enclosing class of wherever the name was written, and that is
 * recovered from the scope chain rather than tracked separately —
 * there is exactly one scope chain and it is already correct, while a
 * second mechanism would be a second thing to keep in step.
 *
 * ---- the three ways in --------------------------------------------------
 *
 * A member of C reaches C's members. A class NESTED in C is a member of
 * C, so it reaches them too ([class.access.nest]) — and this is the
 * rule most hand-written checkers forget, because the nested class is
 * not derived from anything and looks unrelated. A friend reaches them,
 * and friendship is neither inherited nor transitive: a friend of a
 * base is not a friend of the derived class, and a friend of a friend
 * is nobody.
 *
 * `protected` adds one more: a class derived from C reaches C's
 * protected members. [class.protected] narrows that further — the
 * access must be through an object of the derived class, not of C —
 * and that narrowing is NOT enforced here. It needs the type of the
 * object expression at every use, it rejects code rather than accepting
 * it, and getting it wrong is the expensive direction.
 */
#include "cxx.h"

#include "../driver/util.h"

#include <string.h>

static int g_on = 1;

int  access_enabled(void) { return g_on; }
void access_set_enabled(int on) { g_on = on; }

/* Is a diagnostic wanted here at all?
 *
 * Not inside SFINAE. The standard does make access part of the
 * immediate context -- a private constructor is meant to make
 * is_constructible answer false -- but an error raised under cx_sfinae
 * does not reach the user: it unwinds, and silently turns a viable
 * overload into a non-viable one. Getting a check slightly wrong there
 * therefore does not produce a wrong MESSAGE, it produces a different
 * PROGRAM, chosen by a different overload, with nothing said. That is
 * the one failure mode a diagnostic-only feature must not have.
 *
 * Skipping there is also exactly the behaviour of every EmbCC before
 * this file existed, since access was enforced nowhere: nothing that
 * compiles today changes meaning. The cost is that is_constructible
 * and its family still answer as though everything were public, which
 * is a missed diagnostic and stays on the list. */
static int checking_here(void)
{
    return g_on && !cx_sfinae;
}

void access_add_friend_class(struct cclass *c, struct cclass *f)
{
    if (!c || !f)
        return;
    for (int i = 0; i < c->nfrcls; i++)
        if (c->frcls[i] == f)
            return;
    if (c->nfrcls == c->capfrcls) {
        c->capfrcls = c->capfrcls ? c->capfrcls * 2 : 4;
        c->frcls = xrealloc(c->frcls, (size_t)c->capfrcls * sizeof *c->frcls);
    }
    c->frcls[c->nfrcls++] = f;
}

void access_add_friend_func(struct cclass *c, struct cfunc *f)
{
    if (!c || !f)
        return;
    for (int i = 0; i < c->nfrfn; i++)
        if (c->frfn[i] == f)
            return;
    if (c->nfrfn == c->capfrfn) {
        c->capfrfn = c->capfrfn ? c->capfrfn * 2 : 4;
        c->frfn = xrealloc(c->frfn, (size_t)c->capfrfn * sizeof *c->frfn);
    }
    c->frfn[c->nfrfn++] = f;
}

void access_add_friend_template(struct cclass *c, struct ctemplate *t)
{
    if (!c || !t)
        return;
    for (int i = 0; i < c->nfrtmpl; i++)
        if (c->frtmpl[i] == t)
            return;
    if (c->nfrtmpl == c->capfrtmpl) {
        c->capfrtmpl = c->capfrtmpl ? c->capfrtmpl * 2 : 4;
        c->frtmpl = xrealloc(c->frtmpl,
                             (size_t)c->capfrtmpl * sizeof *c->frtmpl);
    }
    c->frtmpl[c->nfrtmpl++] = t;
}

/* Is `inner` lexically inside `outer`? A nested class is a member of
 * the class that encloses it, however deeply. */
static int encloses(struct cclass *outer, struct cclass *inner)
{
    if (!outer || !inner)
        return 0;
    for (struct cscope *s = inner->owner; s; s = s->parent)
        if (s->k == SC_CLASS && s->cls == outer)
            return 1;
    return 0;
}

/* The same entity, for access. Two instances of one class template
 * count as the same -- A<int> reaching A<double>'s privates is wrong
 * and is allowed here on purpose: the friend and member declarations
 * were written once, in the pattern, and telling the instances apart
 * would refuse the member functions of every class template that names
 * its own privates. Granting too much is the safe direction. */
static int same_class(struct cclass *a, struct cclass *b)
{
    if (!a || !b)
        return 0;
    if (a == b)
        return 1;
    if (a->tmpl && a->tmpl == b->tmpl)
        return 1;
    return 0;
}

static int is_friend_of(struct cclass *home, struct cclass *ctx,
                        struct cfunc *fn)
{
    if (!home)
        return 0;
    if (home->befriends_all)
        return 1;
    for (int i = 0; i < home->nfrcls; i++)
        if (ctx && (same_class(home->frcls[i], ctx) ||
                    encloses(home->frcls[i], ctx)))
            return 1;
    /* `template<class U> friend class X;` befriends EVERY instance of
     * X, which is the point of writing it that way -- shared_ptr<T>
     * reaching shared_ptr<U>'s internals is the usual reason. */
    for (int i = 0; i < home->nfrtmpl; i++)
        if (ctx && ctx->tmpl == home->frtmpl[i])
            return 1;
    for (int i = 0; i < home->nfrfn; i++) {
        if (!fn)
            break;
        if (home->frfn[i] == fn)
            return 1;
        /* A friend declared in a class template's pattern and the
         * function actually instantiated are different nodes for the
         * same entity; matching the name and the owning scope is what
         * this front end can tell about them. */
        if (home->frfn[i]->name && fn->name &&
            strcmp(home->frfn[i]->name, fn->name) == 0 &&
            home->frfn[i]->owner == fn->owner)
            return 1;
    }
    return 0;
}

/* The class whose member or friend is naming something here. */
static struct cclass *context_class(void)
{
    for (struct cscope *s = cx_scope; s; s = s->parent)
        if (s->k == SC_CLASS)
            return s->cls;
    return cx_curfn ? cx_curfn->cls : NULL;
}

/* May `ctx` (or the function `fn` in it) reach a member of `home`
 * declared with `acc`? */
static int allowed(struct cclass *home, int acc, struct cclass *ctx,
                   struct cfunc *fn)
{
    if (acc == CA_PUBLIC)
        return 1;
    if (!home)
        return 1;                    /* nothing to be a member of */
    for (struct cclass *k = ctx; k; ) {
        if (same_class(k, home) || encloses(home, k))
            return 1;
        if (acc == CA_PROTECTED && class_derives(k, home, NULL))
            return 1;
        if (is_friend_of(home, k, fn))
            return 1;
        /* A member of a nested class is also inside the enclosing one,
         * and reaches whatever that reaches. */
        struct cclass *up = NULL;
        for (struct cscope *s = k->owner; s; s = s->parent)
            if (s->k == SC_CLASS) { up = s->cls; break; }
        k = up;
    }
    if (is_friend_of(home, NULL, fn))
        return 1;
    return 0;
}

static const char *word(int acc)
{
    return acc == CA_PRIVATE ? "private" : "protected";
}

static void report(struct cclass *home, int acc, const char *what,
                   const char *name, const struct ctok *at)
{
    cx_error(at, "'%s' is %s in '%s'%s", name, word(acc),
             home && home->name ? home->name : "that class", what);
}

/* ---- the entry points --------------------------------------------------
 *
 * Each takes the class the member was FOUND in, which is not always the
 * class that was written: `d.m` where m lives in a base is a member of
 * the base and its access is the base's. */

void access_check_sym(struct cclass *home, const struct csym *y,
                      const char *name, const struct ctok *at)
{
    if (!checking_here() || !y || y->access == CA_PUBLIC)
        return;
    struct cclass *h = y->scope && y->scope->k == SC_CLASS ? y->scope->cls
                                                           : home;
    if (allowed(h, y->access, context_class(), cx_curfn))
        return;
    report(h, y->access, "", name, at);
}

void access_check_func(struct cclass *home, const struct cfunc *f,
                       const char *name, const struct ctok *at)
{
    if (!checking_here() || !f || f->access == CA_PUBLIC)
        return;
    struct cclass *h = f->cls ? f->cls : home;
    if (allowed(h, f->access, context_class(), cx_curfn))
        return;
    /* "'C' is private in 'C'" is what the general wording produces for
     * a constructor, and it says nothing. Name the thing instead. */
    if (f->is_ctor || f->is_dtor)
        cx_error(at, "the %s of '%s' selected here is %s",
                 f->is_ctor ? "constructor" : "destructor",
                 h && h->name ? h->name : "that class", word(f->access));
    else
        report(h, f->access, "", name, at);
}

void access_check_field(struct cclass *home, const struct cfield *fl,
                        const char *name, const struct ctok *at)
{
    if (!checking_here() || !fl || fl->access == CA_PUBLIC)
        return;
    if (allowed(home, fl->access, context_class(), cx_curfn))
        return;
    report(home, fl->access, "", name, at);
}

/* [class.access.base]: converting D to B needs a path on which EVERY
 * edge is accessible from here. One edge is not enough to ask about --
 * `D : public M` and `M : private B` leaves B unreachable from outside
 * though the only edge D has is public -- so this walks, and an edge
 * that is closed simply is not followed. If some other path is open,
 * the conversion is fine; that is what "accessible" means. */
static int base_reachable(struct cclass *d, struct cclass *b,
                          struct cclass *ctx, struct cfunc *fn, int depth)
{
    if (same_class(d, b))
        return 1;
    if (depth > 32)
        return 1;                    /* a cycle cannot happen; be safe */
    for (int i = 0; i < d->nbases; i++) {
        int acc = d->bases[i].access;
        if (acc != CA_PUBLIC && !allowed(d, acc, ctx, fn))
            continue;
        if (base_reachable(d->bases[i].cls, b, ctx, fn, depth + 1))
            return 1;
    }
    return 0;
}

int access_base_ok(struct cclass *d, struct cclass *b)
{
    if (!checking_here() || !d || !b || same_class(d, b))
        return 1;
    return base_reachable(d, b, context_class(), cx_curfn, 0);
}
