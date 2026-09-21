/* EmbCC C++ front end: coroutines (C++20 [dcl.fct.def.coroutine],
 * [expr.await], [stmt.return.coroutine]). A function with co_await,
 * co_yield or co_return is a coroutine: its promise type is
 * std::coroutine_traits<R, Params...>::promise_type, and what the
 * lowering runs — the promise's construction, get_return_object(), the
 * initial and final suspends, unhandled_exception(), the frame's
 * allocation — is built here, as expressions, the first time one of
 * those keywords is met. emit.c turns the function into its frame, the
 * ramp (the function itself) and one body resumed and destroyed through
 * the frame. */
#include "cxx.h"
#include "../driver/util.h"
#include <string.h>

/* std::name (or in an inline namespace of std: std::__n4861) */
static struct ctemplate *std_class_template(const char *name)
{
    struct csym *ns = scope_find_here(cx_global, "std");
    if (!ns || ns->k != CS_NAMESPACE)
        return NULL;
    struct csym *y = lookup_in(ns->ns, name);
    return y && y->k == CS_TEMPLATE && y->tmpl->kind == TK_CLASS ? y->tmpl
                                                                 : NULL;
}

static struct ctarg type_targ(struct cty *t)
{
    struct ctarg a;
    memset(&a, 0, sizeof a);
    a.kind = TP_TYPE;
    a.type = t;
    return a;
}

/* e, an lvalue, as an xvalue (std::move(e)) */
static struct cexpr *as_xvalue(struct cexpr *e)
{
    struct cexpr *x = ex_new(E_CAST, e->t, VC_XVALUE);
    x->a = xmalloc(sizeof *x->a);
    x->a[0] = e;
    x->na = 1;
    x->lvcast = 1;
    return x;
}

/* A variable the lowering keeps in the frame (emit.c names it there,
 * unless cname is given: an expression of the frame itself). */
static struct cvar *frame_var(const char *cname, struct cty *t,
                              const struct ctok *at)
{
    struct cvar *v = cx_new_local(NULL, t, at);
    if (cname)
        v->cname = cname;
    return v;
}

static int promise_has(struct ccoro *co, const char *name)
{
    struct csym *y = class_member(co->promise_t->cls, name);
    return y && y->k == CS_FUNC;
}

static struct cexpr *promise_call(struct ccoro *co, const char *name,
                                  struct cexpr **args, int na,
                                  const struct ctok *at)
{
    if (!promise_has(co, name))
        cx_error(at, "the promise type '%s' has no member '%s'",
                 ct_name(co->promise_t), name);
    return expr_call_named(expr_var(co->promise), name, args, na, at);
}

/* std::coroutine_handle<P>::from_address(the frame) */
static struct cexpr *handle_of(struct ccoro *co, const struct ctok *at)
{
    struct ctemplate *ht = std_class_template("coroutine_handle");
    if (!ht)
        cx_error(at, "std::coroutine_handle is not declared (#include "
                     "<coroutine>)");
    struct ctarg a = type_targ(co->promise_t);
    struct cclass *hc = class_instance(ht, &a, 1, at);
    struct cexpr *fp = rvalue(expr_var(co->fp));
    return expr_call_static(hc, "from_address", &fp, 1, at);
}

/* The allocation function the frame comes from: the promise type's
 * (given the size and the parameters, else the size alone), else the
 * global one — the nothrow one when the promise has
 * get_return_object_on_allocation_failure. */
static struct cexpr *frame_alloc(struct ccoro *co, struct cfunc *f,
                                 struct cvar *size, int nothrow,
                                 const struct ctok *at)
{
    struct cexpr *sz = rvalue(expr_var(size));
    struct csym *y = scope_find_here(co->promise_t->cls->scope,
                                     "operator new");
    if (y && y->k == CS_FUNC) {
        int np = f->type->np;
        struct cexpr **args = xmalloc((size_t)(np + 1) * sizeof *args);
        args[0] = sz;
        for (int i = 0; i < np; i++)
            args[i + 1] = expr_var(f->params[i]);
        struct cfunc *fn = np ? resolve(y->fns, NULL, args, np + 1, NULL,
                                        "operator new") : NULL;
        if (fn)
            return make_call(fn, NULL, args, np + 1, at);
        fn = resolve(y->fns, NULL, &sz, 1, at, "operator new");
        return make_call(fn, NULL, &sz, 1, at);
    }
    if (nothrow) {
        struct csym *ns = scope_find_here(cx_global, "std");
        struct csym *nt = ns && ns->k == CS_NAMESPACE
                          ? lookup_in(ns->ns, "nothrow") : NULL;
        if (!nt || nt->k != CS_VAR)
            cx_error(at, "std::nothrow is not declared (#include <new>)");
        struct cexpr *args[2] = { sz, expr_var(nt->var) };
        return call_global_op("operator new", args, 2, at);
    }
    return call_global_op("operator new", &sz, 1, at);
}

/* ... and the deallocation function: the promise type's, else the
 * global one — each with the size if it takes it. */
static struct cexpr *frame_dealloc(struct ccoro *co, struct cvar *size,
                                   const struct ctok *at)
{
    struct cexpr *args[2] = { rvalue(expr_var(co->fp)),
                              rvalue(expr_var(size)) };
    struct csym *y = scope_find_here(co->promise_t->cls->scope,
                                     "operator delete");
    if (!y || y->k != CS_FUNC)
        y = lookup_in(cx_global, "operator delete");
    if (!y || y->k != CS_FUNC)
        cx_error(at, "'operator delete' is not declared");
    struct cfunc *fn = resolve(y->fns, NULL, args, 1, NULL,
                               "operator delete");
    int na = 1;
    if (!fn) {
        fn = resolve(y->fns, NULL, args, 2, at, "operator delete");
        na = 2;
    }
    return make_call(fn, NULL, args, na, at);
}

struct ccoro *coro_of(const struct ctok *at)
{
    struct cfunc *f = cx_curfn;
    if (!f)
        cx_error(at, "co_await, co_yield and co_return belong in a "
                     "function's body");
    if (f->coro)
        return f->coro;
    if (f->is_ctor || f->is_dtor)
        cx_error(at, "a constructor or destructor cannot be a coroutine");
    if (ct_has_auto(f->type->to))
        cx_error(at, "a coroutine's return type cannot be deduced");
    if (f->type->variadic)
        cx_error(at, "a coroutine cannot take a C variable argument list");
    if (f->is_constexpr)
        cx_error(at, "a constexpr function cannot be a coroutine");
    if (!f->cls && f->owner == cx_global && strcmp(f->name, "main") == 0)
        cx_error(at, "main cannot be a coroutine");
    struct ctemplate *tr = std_class_template("coroutine_traits");
    if (!tr)
        cx_error(at, "std::coroutine_traits is not declared (#include "
                     "<coroutine>)");

    /* the promise type: coroutine_traits<R, [cv X &,] Params...> */
    int np = f->type->np;
    int self = f->this_var != NULL;
    struct ctarg *ta = xcalloc((size_t)np + 2, sizeof *ta);
    int n = 0;
    ta[n++] = type_targ(f->type->to);
    if (self)
        ta[n++] = type_targ(ct_ref(f->this_var->type->to,
                                   f->type->refq == 2));
    for (int i = 0; i < np; i++)
        ta[n++] = type_targ(f->type->params[i]);
    struct cclass *tc = class_instance(tr, ta, n, at);
    class_ensure(tc);
    struct csym *py = tc->complete ? class_member(tc, "promise_type") : NULL;
    struct cty *P = py && (py->k == CS_TYPEDEF || py->k == CS_CLASS)
                    ? ct_unqual(py->type) : NULL;
    if (!P || P->k != CT_CLASS)
        cx_error(at, "'%s' has no promise_type: not a coroutine's return "
                     "type", ct_name(f->type->to));
    class_ensure(P->cls);
    if (!P->cls->complete)
        cx_error(at, "the promise type '%s' is incomplete", ct_name(P));

    struct ccoro *co = xcalloc(1, sizeof *co);
    f->coro = co;
    co->frame = cx_fmt("__cx_cf%d", cx_uid());
    co->promise_t = P;
    co->promise = frame_var("__cx_fr->__p", P, at);
    co->fp = frame_var("((void *)__cx_fr)", ct_ptr(ct_basic(CT_VOID)), at);
    struct cvar *size = frame_var(cx_fmt("sizeof(struct %s)", co->frame),
                                  ct_size_t(), at);

    /* the parameters' copies: a class not trivially copyable moved from
     * the parameter (the rest copied as C copies) */
    co->pcopy = xcalloc((size_t)(np ? np : 1), sizeof *co->pcopy);
    for (int i = 0; i < np; i++) {
        struct cty *pt = f->params[i]->type;
        if (!ct_is_ref(pt) && pt->k == CT_CLASS && class_indirect(pt)) {
            struct cexpr *x = as_xvalue(expr_var(f->params[i]));
            co->pcopy[i] = construct(ct_unqual(pt)->cls, INIT_DIRECT, &x, 1,
                                     at);
        }
    }

    /* the promise: from the copies (and the object) if a constructor
     * takes them, else value-initialized */
    struct cexpr **pa = xmalloc((size_t)(np + 1) * sizeof *pa);
    int npa = 0;
    if (self)
        pa[npa++] = expr_deref(ex_this());
    for (int i = 0; i < np; i++)
        pa[npa++] = expr_var(f->params[i]);
    struct cclass *pc = P->cls;
    if (npa && pc->user_ctors &&
        resolve_ex(pc->ctors, NULL, pa, npa, NULL, NULL, 0))
        co->promise_init = construct(pc, INIT_DIRECT, pa, npa, at);
    else
        co->promise_init = construct(pc, INIT_VALUE, NULL, 0, at);

    struct cexpr *g = promise_call(co, "get_return_object", NULL, 0, at);
    struct cty *R = f->type->to;
    co->get_ro = R->k == CT_VOID ? g
                 : ct_is_ref(R) ? bind_ref(g, R, "get_return_object()")
                 : init_object(ct_unqual(R), INIT_COPY, &g, 1, at);
    co->init_susp = coro_await(promise_call(co, "initial_suspend", NULL, 0,
                                            at), 2, at);
    co->final_susp = coro_await(promise_call(co, "final_suspend", NULL, 0,
                                             at), 3, at);
    if (cx_exceptions)
        co->unhandled = promise_call(co, "unhandled_exception", NULL, 0, at);
    if (promise_has(co, "return_void"))
        co->ret_void = promise_call(co, "return_void", NULL, 0, at);
    struct csym *fy = class_member(pc, "get_return_object_on_allocation_"
                                       "failure");
    if (fy && fy->k == CS_FUNC) {
        struct cexpr *g2 = expr_call_static(pc, "get_return_object_on_"
                                                "allocation_failure", NULL,
                                            0, at);
        co->alloc_fail = R->k == CT_VOID ? g2
                         : init_object(ct_unqual(R), INIT_COPY, &g2, 1, at);
    }
    co->alloc = frame_alloc(co, f, size, co->alloc_fail != NULL, at);
    co->dealloc = frame_dealloc(co, size, at);
    return co;
}

/* co_await e (kind 0), or the await of what the promise gave: co_yield's
 * yield_value (1), the initial (2) and the final (3) suspend — no
 * await_transform for those. The awaitable's operator co_await (member
 * or not) gives the awaiter, kept in the frame: an object (a prvalue),
 * else a reference. */
struct cexpr *coro_await(struct cexpr *e, int kind, const struct ctok *at)
{
    struct ccoro *co = coro_of(at);
    if (kind == 0 && promise_has(co, "await_transform"))
        e = promise_call(co, "await_transform", &e, 1, at);
    struct cexpr *aw = NULL;
    if (e->t && e->t->k == CT_CLASS)
        aw = expr_operator_call("operator co_await", &e, 1, at);
    if (!aw)
        aw = e;
    struct cty *awt = ct_unqual(aw->t);
    if (awt->k != CT_CLASS)
        cx_error(at, "'%s' is not awaitable (no await_ready, await_suspend "
                     "and await_resume)", ct_name(aw->t));
    class_ensure(awt->cls);
    struct cvar *v;
    struct cexpr *init;
    if (aw->vc == VC_PRVALUE) {
        v = frame_var(NULL, awt, at);
        init = v->ctor = aw;
    } else {
        struct cty *rt = ct_ref(aw->t, aw->vc == VC_XVALUE);
        v = frame_var(NULL, rt, at);
        init = v->init = bind_ref(aw, rt, "co_await");
    }
    struct cexpr *ready = convert_bool(expr_call_named(expr_var(v),
                                                       "await_ready", NULL,
                                                       0, at),
                                       "await_ready()");
    struct cexpr *h = handle_of(co, at);
    struct cexpr *susp = expr_call_named(expr_var(v), "await_suspend", &h, 1,
                                         at);
    int sk;
    struct cty *st = ct_unqual(susp->t);
    if (st->k == CT_VOID) {
        sk = 0;
    } else if (st->k == CT_BOOL) {
        sk = 1;
    } else if (st->k == CT_CLASS) {
        /* a coroutine_handle to resume instead: its address */
        sk = 2;
        susp = expr_call_named(susp, "address", NULL, 0, at);
    } else {
        cx_error(at, "await_suspend returns '%s' (void, bool or a "
                     "coroutine_handle)", ct_name(susp->t));
        return NULL;
    }
    struct cexpr *res = expr_call_named(expr_var(v), "await_resume", NULL, 0,
                                        at);
    struct cexpr *r = ex_new(E_COAWAIT, res->t, res->vc);
    r->line = at->t.line;
    r->file = at->file;
    r->var = v;
    r->a = xmalloc(4 * sizeof *r->a);
    r->a[0] = init;
    r->a[1] = ready;
    r->a[2] = susp;
    r->a[3] = res;
    r->na = 4;
    r->ival = sk;
    r->coro = kind == 2 ? 1 : kind == 3 ? 2 : 0;
    return r;
}

/* co_yield e: co_await promise.yield_value(e) */
struct cexpr *coro_yield(struct cexpr *e, const struct ctok *at)
{
    struct ccoro *co = coro_of(at);
    return coro_await(promise_call(co, "yield_value", &e, 1, at), 1, at);
}

/* co_return [e]: promise.return_value(e) — a local named alone moved
 * from — or (e void or absent) promise.return_void(). */
struct cexpr *coro_return(struct cexpr *e, int braced, const struct ctok *at)
{
    struct ccoro *co = coro_of(at);
    if (e && (braced || e->t->k != CT_VOID)) {
        if (e->k == E_VAR && e->var->is_local && !e->var->is_static &&
            !ct_is_ref(e->var->type) && e->vc == VC_LVALUE)
            e = as_xvalue(e);
        return promise_call(co, "return_value", &e, 1, at);
    }
    struct cexpr *rv = promise_call(co, "return_void", NULL, 0, at);
    if (!e)
        return rv;
    struct cexpr *c = ex_new(E_COMMA, rv->t, rv->vc);
    c->a = xmalloc(2 * sizeof *c->a);
    c->a[0] = e;
    c->a[1] = rv;
    c->na = 2;
    return c;
}
