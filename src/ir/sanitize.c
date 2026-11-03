/* -fsanitize=address, thread and memory: the instrumentation. The
 * runtimes are lib/san (libasan.a, libtsan.a, libmsan.a); this is what
 * calls them.
 *
 * AddressSanitizer: every load and store is checked against a shadow of
 * memory that marks what is addressable. The check is a call --
 * __asan_load4(addr), __asan_storeN(addr, size) -- as LLVM's
 * instrumentation makes it with -asan-instrumentation-with-call-
 * threshold=0: one form for every target, a board's RAM included, where
 * the runtime decides what the shadow covers. Around what can be
 * overrun there are redzones: a heap block's are the runtime's (malloc
 * and friends are renamed to it), a global's are bytes after it that the
 * runtime poisons when the unit's constructor registers it, and a
 * local's are carved with it -- the locals that could be overrun (an
 * array, a struct, anything whose address is taken) are taken off the
 * frame into one block with redzones between them (sema marks them,
 * irgen carves the block), poisoned on entry and made good again before
 * each return. A call that does not return (longjmp, a throw, exit)
 * first tells the runtime, which unpoisons the stack it leaves.
 *
 * ThreadSanitizer: every load and store reports itself (__tsan_read4,
 * __tsan_write_range); an atomic one, or a call of the atomic library,
 * is a release before and an acquire after instead; each function says
 * when it is entered and left, for the reports' stacks; and the
 * synchronization calls -- pthreads, C11 threads -- and malloc are
 * renamed to the runtime's, which records the happens-before they make.
 *
 * MemorySanitizer: the uninitialized-read checker. Its own pass, below.
 *
 * All of it runs after the optimizer, on the code that will be emitted,
 * so what it checks is what runs and it costs the optimizer nothing. */

#include "ir.h"
#include "irgen_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../driver/util.h"
#include "../sema/sema.h"
#include "../sema/type.h"
#include "../arch/target.h"

static unsigned g_rt;
static int g_recover;

void san_set_runtime(unsigned mask, int recover)
{
    g_rt = mask & (SAN_ADDRESS | SAN_THREAD | SAN_MEMORY);
    g_recover = recover;
}

unsigned san_runtime(void) { return g_rt; }

static int func_wants(const struct func *f, unsigned bit)
{
    if (!(g_rt & bit) || !f)
        return 0;
    if (f->attr_no_san & bit)
        return 0;
    if (f->is_naked)
        return 0;
    const char *n = f->name;
    return strncmp(n, "__asan_", 7) && strncmp(n, "__tsan_", 7) &&
           strncmp(n, "__msan_", 7) && strncmp(n, "__embcc_", 8);
}

/* ---- sema: the locals AddressSanitizer gives redzones ------------------ */

static void walk_stmt(struct stmt *s, char *m, int n);

static int base_var(struct expr *e)
{
    while (e && (e->kind == EXPR_MEMBER) && !e->is_arrow)
        e = e->lhs;
    while (e && e->kind == EXPR_BINOP && e->is_subscript)
        e = e->lhs;
    return e && e->kind == EXPR_VAR ? e->var_index : -1;
}

static void walk_expr(struct expr *e, char *m, int n)
{
    if (!e)
        return;
    if (e->kind == EXPR_ADDR) {
        int v = base_var(e->rhs);
        if (v >= 0 && v < n)
            m[v] = 1;
    }
    walk_expr(e->lhs, m, n);
    walk_expr(e->rhs, m, n);
    for (int k = 0; e->args && k < e->nargs; k++)
        walk_expr(e->args[k], m, n);
    for (int k = 0; e->elems && k < e->nelems; k++)
        walk_expr(e->elems[k], m, n);
    for (int k = 0; e->inits && k < e->ninits; k++)
        walk_expr(e->inits[k].e, m, n);
    for (int k = 0; e->gexprs && k < e->ngen; k++)
        walk_expr(e->gexprs[k], m, n);
    if (e->kind == EXPR_STMTEXPR)
        walk_stmt(e->body, m, n);
}

static void walk_stmt(struct stmt *s, char *m, int n)
{
    for (; s; s = s->next) {
        walk_expr(s->expr, m, n);
        walk_expr(s->cond, m, n);
        walk_expr(s->init, m, n);
        walk_expr(s->step, m, n);
        for (int k = 0; s->inits && k < s->ninits; k++)
            walk_expr(s->inits[k].e, m, n);
        walk_stmt(s->initdecl, m, n);
        walk_stmt(s->thn, m, n);
        walk_stmt(s->els, m, n);
        walk_stmt(s->body, m, n);
    }
}

/* Which of f's locals (from first on, nvars of them) get redzones: the
 * aggregates, and the scalars whose address is taken. 1 for each, in a
 * new array; NULL when -fsanitize=address is off for f. */
char *san_asan_locals(struct func *f, int nvars)
{
    if (!func_wants(f, SAN_ADDRESS) || !f->body || nvars <= 0)
        return NULL;
    char *m = xcalloc((size_t)nvars, 1);
    walk_stmt(f->body, m, nvars);
    for (int v = 0; v < nvars; v++) {
        const struct type *t = f->var_tys[v];
        if (t && (t->kind == TY_ARRAY || t->kind == TY_STRUCT))
            m[v] = 1;
    }
    return m;
}

/* ---- after the optimizer ------------------------------------------------ */

struct ed {
    struct ir_unit *iu;
    struct ir_func *fn;
    struct ir_slice *sl;
    int ns, cap;
};

static int put_const(struct ir_func *fn, long v)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_CONST;
    i->imm = v;
    i->w = target_ptr_size() == 8 ? 8 : 4;
    i->dst = fn->nvregs++;
    return i->dst;
}

/* a call of the runtime, before old instruction `at` */
static void call_at(struct ed *e, int at, const char *name, int n,
                    const int *vals, long size, int line)
{
    if (e->ns == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 64;
        e->sl = xrealloc(e->sl, (size_t)e->cap * sizeof *e->sl);
    }
    struct ir_slice *s = &e->sl[e->ns++];
    s->at = at;
    s->lo = e->fn->nins;
    int v[3] = { n > 0 ? vals[0] : -1, n > 1 ? vals[1] : -1, -1 };
    if (size >= 0)
        v[n++] = put_const(e->fn, size);
    irgen_san_call(e->iu, e->fn, name, n, v, line);
    s->hi = e->fn->nins;
}

static const char *sized(const char *pre, int size, const char *suf)
{
    static char b[64];
    snprintf(b, sizeof b, "%s%d%s", pre, size, suf);
    return xstrndup(b, strlen(b));
}

static int pow2_size(long s)
{
    return s == 1 || s == 2 || s == 4 || s == 8 || s == 16;
}

/* an access of `size` bytes at vreg a: ASan's check, or TSan's report */
static void access(struct ed *e, int at, int a, long size, int write,
                   int line, unsigned bit)
{
    if (size <= 0)
        return;
    if (bit == SAN_ADDRESS) {
        const char *suf = g_recover ? "_noabort" : "";
        if (pow2_size(size))
            call_at(e, at, sized(write ? "__asan_store" : "__asan_load",
                                 (int)size, suf), 1, &a, -1, line);
        else
            call_at(e, at, write ? (g_recover ? "__asan_storeN_noabort"
                                              : "__asan_storeN")
                                 : (g_recover ? "__asan_loadN_noabort"
                                              : "__asan_loadN"),
                    1, &a, size, line);
    } else {
        if (pow2_size(size))
            call_at(e, at, sized(write ? "__tsan_write" : "__tsan_read",
                                 (int)size, ""), 1, &a, -1, line);
        else
            call_at(e, at, write ? "__tsan_write_range" : "__tsan_read_range",
                    1, &a, size, line);
    }
}

/* the C library calls a sanitizer has its own of */
static const char *const asan_names[] = {
    "malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc",
    "memalign", "valloc", "pvalloc", "strdup", "strndup",
    "memcpy", "memmove", "memset", "strcpy", "strncpy", "strcat", "strncat",
    "strlen", "strnlen", NULL
};
static const char *const tsan_names[] = {
    "pthread_create", "pthread_join", "pthread_detach",
    "pthread_mutex_lock", "pthread_mutex_trylock", "pthread_mutex_unlock",
    "pthread_mutex_timedlock",
    "pthread_rwlock_rdlock", "pthread_rwlock_wrlock",
    "pthread_rwlock_tryrdlock", "pthread_rwlock_trywrlock",
    "pthread_rwlock_unlock",
    "pthread_spin_lock", "pthread_spin_trylock", "pthread_spin_unlock",
    "pthread_cond_wait", "pthread_cond_timedwait", "pthread_cond_signal",
    "pthread_cond_broadcast", "pthread_once", "pthread_barrier_wait",
    "sem_wait", "sem_trywait", "sem_timedwait", "sem_post",
    "thrd_create", "thrd_join", "thrd_detach", "mtx_lock", "mtx_trylock",
    "mtx_timedlock", "mtx_unlock", "cnd_wait", "cnd_timedwait",
    "cnd_signal", "cnd_broadcast", "call_once",
    "malloc", "calloc", "realloc", "free", "posix_memalign",
    "aligned_alloc", "strdup", "strndup",
    "memcpy", "memmove", "memset", NULL
};

static const char *const msan_names[] = {
    "malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc",
    "memalign", "valloc", "strdup", "strndup",
    "memcpy", "memmove", "memset", "bzero", "strcpy", "strncpy", "stpcpy",
    "strcat", "strncat",
    "read", "pread", "fread", "fgets", "getline", "getdelim",
    "sprintf", "snprintf", "vsprintf", "vsnprintf",
    "scanf", "sscanf", "fscanf", "vscanf", "vsscanf", "vfscanf",
    "stat", "fstat", "lstat", "time", "gettimeofday", "clock_gettime",
    "pipe", "localtime_r", "gmtime_r", "strftime", "getcwd", "realpath",
    "readlink", "recv", "recvfrom", "socketpair", "frexp", "modf",
    "strtol", "strtoul", "strtoll", "strtoull", "strtod", "strtof",
    NULL
};

static struct func *renamed(struct func *f, const char *pre,
                            const char *const *names)
{
    if (!f || f->has_defn)
        return NULL;
    for (int k = 0; names[k]; k++)
        if (!strcmp(f->name, names[k])) {
            char b[96];
            snprintf(b, sizeof b, "%s%s", pre, f->name);
            return irgen_func_renamed(f, xstrndup(b, strlen(b)));
        }
    return NULL;
}

/* a call that does not come back to this frame */
static int no_return(const struct ir_ins *i)
{
    if (i->indirect || !i->callee)
        return 0;
    const char *n = i->callee->name;
    return i->callee->is_noreturn || !strcmp(n, "longjmp") ||
           !strcmp(n, "_longjmp") || !strcmp(n, "siglongjmp") ||
           !strcmp(n, "__builtin_longjmp") || !strcmp(n, "__cxa_throw") ||
           !strcmp(n, "__cxa_rethrow") || !strcmp(n, "_Unwind_Resume") ||
           !strcmp(n, "_Unwind_RaiseException");
}

static void instrument(struct ir_unit *iu, struct ir_func *fn, unsigned bit)
{
    struct ed e = { iu, fn, NULL, 0, 0 };
    int n0 = fn->nins;
    const char *const *names = bit == SAN_ADDRESS ? asan_names : tsan_names;
    const char *pre = bit == SAN_ADDRESS ? "__embcc_asan_" : "__embcc_tsan_";
    if (bit == SAN_THREAD) {
        /* the function, for the reports' stacks */
        struct ir_ins *fa = emit(fn);
        fa->op = IR_FADDR;
        fa->callee = fn->src;
        fa->callee_sym = ir_sym_func(iu, fn->src);
        fa->dst = fn->nvregs++;
        fa->w = target_ptr_size() == 8 ? 8 : 4;
        int self = fa->dst;
        if (e.ns == e.cap) {
            e.cap = 64;
            e.sl = xrealloc(e.sl, (size_t)e.cap * sizeof *e.sl);
        }
        e.sl[e.ns].at = 0;
        e.sl[e.ns].lo = n0;
        e.sl[e.ns].hi = fn->nins;
        e.ns++;
        call_at(&e, 0, "__tsan_func_entry", 1, &self, -1,
                n0 ? fn->ins[0].line : 0);
    }
    for (int n = 0; n < n0; n++) {
        struct ir_ins *i = &fn->ins[n];
        int line = i->line, a = i->a;
        switch (i->op) {
        case IR_LOAD:
            if (i->flash)
                break;
            if (bit == SAN_THREAD && i->atomic)
                call_at(&e, n + 1, "__embcc_tsan_acquire", 1, &a, -1, line);
            else
                access(&e, n, a, i->size, 0, line, bit);
            break;
        case IR_STORE:
            if (bit == SAN_THREAD && i->atomic)
                call_at(&e, n, "__embcc_tsan_release", 1, &a, -1, line);
            else
                access(&e, n, a, i->size, 1, line, bit);
            break;
        case IR_VLOAD:
            access(&e, n, a, 16, 0, line, bit);
            break;
        case IR_VSTORE:
            access(&e, n, a, 16, 1, line, bit);
            break;
        case IR_XCHG: case IR_XADD: case IR_ARMW: case IR_CAS:
        case IR_CAS16: case IR_CMPXCHG:
            if (bit == SAN_THREAD) {
                call_at(&e, n, "__embcc_tsan_release", 1, &a, -1, line);
                call_at(&e, n + 1, "__embcc_tsan_acquire", 1, &a, -1, line);
            } else {
                access(&e, n, a, i->op == IR_CAS16 ? 16 : i->size, 1, line,
                       bit);
                if (i->op == IR_CMPXCHG)
                    access(&e, n, i->b, i->size, 1, line, bit);
            }
            break;
        case IR_MEMCPY:
            access(&e, n, a, i->size, 1, line, bit);
            access(&e, n, i->b, i->size, 0, line, bit);
            break;
        case IR_MEMZERO:
            access(&e, n, a, i->size, 1, line, bit);
            break;
        case IR_FADDR: {
            struct func *r = renamed(i->callee, pre, names);
            if (r) {
                i->callee = r;
                i->callee_sym = ir_sym_func(iu, r);
            }
            break;
        }
        case IR_CALL: {
            struct func *r = i->indirect ? NULL : renamed(i->callee, pre, names);
            if (r) {
                i->callee = r;
                i->callee_sym = ir_sym_func(iu, r);
            }
            if (bit == SAN_ADDRESS && no_return(i))
                call_at(&e, n, "__asan_handle_no_return", 0, NULL, -1, line);
            if (bit == SAN_THREAD && !i->indirect && i->callee &&
                (!strncmp(i->callee->name, "__atomic_", 9) ||
                 !strncmp(i->callee->name, "__sync_", 7)) &&
                i->nargs > 0 && !i->argv[0].is_struct) {
                int p = i->argv[0].vreg;
                call_at(&e, n, "__embcc_tsan_release", 1, &p, -1, line);
                call_at(&e, n + 1, "__embcc_tsan_acquire", 1, &p, -1, line);
            }
            if (bit == SAN_THREAD && no_return(i))
                call_at(&e, n, "__tsan_func_exit", 0, NULL, -1, line);
            break;
        }
        case IR_RET:
            if (bit == SAN_THREAD)
                call_at(&e, n, "__tsan_func_exit", 0, NULL, -1, line);
            break;
        default:
            break;
        }
    }
    if (e.ns) {
        int end = fn->nins;
        ir_splice(fn, n0, end, e.sl, e.ns);
    }
    free(e.sl);
}

static void msan_func(struct ir_unit *iu, struct ir_func *fn);

void san_instrument(struct ir_unit *iu)
{
    if (!g_rt)
        return;
    for (int k = 0; k < iu->nfuncs; k++) {
        struct ir_func *fn = &iu->funcs[k];
        if (!fn->src || fn->nins == 0)
            continue;
        if (func_wants(fn->src, SAN_ADDRESS))
            instrument(iu, fn, SAN_ADDRESS);
        if (func_wants(fn->src, SAN_THREAD))
            instrument(iu, fn, SAN_THREAD);
        if (func_wants(fn->src, SAN_MEMORY))
            msan_func(iu, fn);
    }
}

/* ---- AddressSanitizer's globals ----------------------------------------
 *
 * Each global the program defines gets a redzone after it -- at least 32
 * bytes, and up to the next multiple of 32 -- and is 32-aligned, and the
 * unit's constructor registers the table of them, {address, size, size
 * with the redzone} each, with __embcc_asan_register_globals. Not one in
 * a section of its own (a linker-assembled table such a section often
 * is would be broken by the gaps), a thread's, a common or weak one,
 * an alias, or the runtime's. */
const char *san_ctor_source(void)
{
    if (!(g_rt & SAN_ADDRESS))
        return "";
    return "\n# 1 \"<asan>\"\n"
           "static unsigned char __embcc_asan_globals[8] "
           "__attribute__((used)) = { 0 };\n"
           "void __embcc_asan_register_globals(void *);\n"
           "static void __attribute__((constructor, used, "
           "no_instrument_function, no_profile_instrument_function, "
           "no_sanitize(\"address\"))) __embcc_asan_ctor(void) "
           "{ __embcc_asan_register_globals(__embcc_asan_globals); }\n";
}

void san_globals(struct unit *u)
{
    if (!(g_rt & SAN_ADDRESS))
        return;
    struct global *tab = NULL;
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && !strcmp(g->name, "__embcc_asan_globals"))
            tab = g;
    if (!tab)
        return;
    int P = target_ptr_size(), be = target_big_endian();
    int n = 0, cap = 0;
    struct global **gs = NULL;
    long *orig = NULL;
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined || g->is_extern || g->is_tls ||
            g->section || g->is_common || g->is_weak || g->alias_of ||
            !strncmp(g->name, "__embcc_", 8) || !g->ty)
            continue;
        long size = global_size(g);
        if (size <= 0)
            continue;
        long rz = 32 + (32 - size % 32) % 32;
        g->fam_extra += (int)rz;
        if (g->user_align < 32)
            g->user_align = 32;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            gs = xrealloc(gs, (size_t)cap * sizeof *gs);
            orig = xrealloc(orig, (size_t)cap * sizeof *orig);
        }
        orig[n] = size;
        gs[n++] = g;
    }
    size_t len = (size_t)P * (1 + 3 * (size_t)n);
    unsigned char *b = xcalloc(len ? len : 1, 1);
    struct greloc *rel = xcalloc((size_t)(n ? n : 1), sizeof *rel);
#define PUT(at, v) do {                                                    \
        unsigned long long v_ = (unsigned long long)(v);                   \
        for (int k_ = 0; k_ < P; k_++)                                     \
            b[(at) + (size_t)k_] = (unsigned char)(v_ >> (8 * (be ? P - 1 - k_ \
                                                                    : k_))); \
    } while (0)
    PUT(0, n);
    for (int k = 0; k < n; k++) {
        size_t at = (size_t)P * (1 + 3 * (size_t)k);
        rel[k].off = (int)at;
        rel[k].gtarget = gs[k];
        PUT(at + (size_t)P, orig[k]);
        PUT(at + 2 * (size_t)P, global_size(gs[k]));
    }
#undef PUT
    tab->ty = ty_array(ty_base(TY_CHAR, 1), (int)len);
    tab->user_align = P;
    tab->has_init = 1;
    tab->init_bytes = (const char *)b;
    tab->init_len = (int)len;
    tab->init_expr = NULL;
    tab->relocs = rel;
    tab->nrelocs = n;
    free(gs);
    free(orig);
}

/* ---- MemorySanitizer -----------------------------------------------------
 *
 * Every value has a shadow of the same width, its uninitialized bits; a
 * vreg's is another vreg, memory's -- the frame's slots as well -- is the
 * runtime's, one shadow byte per byte (lib/san/msan.c). The shadows go
 * with the values: an operation's is its operands' together (AND and OR
 * exactly: a bit known 0 in one operand of an AND is initialized however
 * the other is), a load's the memory's, a call's arguments and result go
 * through the runtime's parameter slots, which a callee not instrumented
 * leaves alone (its result is then initialized). A value is CHECKED
 * where it decides something -- a branch, a switch, a computed goto, an
 * address, a divisor -- and a poisoned one there is the report.
 *
 * A function's locals are poisoned when it is entered, a malloc'd block
 * when it is made; what the C library writes into either is made good by
 * the runtime's versions of those functions. A function computing in
 * __int128, long double or vectors is left alone: its values are taken
 * as initialized (no report it could be wrong about). */

struct ms {
    struct ed e;
    int *sh;                    /* vreg -> its shadow vreg, or -1 */
    int nsh;
};

static int ms_new(struct ir_func *fn) { return fn->nvregs++; }

static int ms_op(struct ir_func *fn, enum ir_op op, int a, int b, int w,
                 int sign)
{
    struct ir_ins *i = emit(fn);
    i->op = op;
    i->a = a;
    i->b = b;
    i->w = w;
    i->sign = sign;
    i->dst = ms_new(fn);
    return i->dst;
}

static int ms_const(struct ir_func *fn, long v, int w)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_CONST;
    i->imm = v;
    i->w = w;
    i->dst = ms_new(fn);
    return i->dst;
}

/* the shadow vreg of v (made on first sight) */
static int S(struct ms *m, int v)
{
    if (v < 0 || v >= m->nsh)
        return -1;
    if (m->sh[v] < 0)
        m->sh[v] = ms_new(m->e.fn);
    return m->sh[v];
}

/* x (width w) as an address-sized argument */
static int ms_arg(struct ir_func *fn, int x, int w)
{
    int P = target_ptr_size();
    if (w >= P)
        return x;
    struct ir_ins *i = emit(fn);
    i->op = IR_EXT;
    i->a = x;
    i->size = w;
    i->sign = 0;
    i->w = P;
    i->dst = ms_new(fn);
    return i->dst;
}

/* begin a slice before old instruction `at` */
static struct ir_slice *ms_open(struct ms *m, int at)
{
    struct ed *e = &m->e;
    if (e->ns == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 64;
        e->sl = xrealloc(e->sl, (size_t)e->cap * sizeof *e->sl);
    }
    struct ir_slice *s = &e->sl[e->ns++];
    s->at = at;
    s->lo = e->fn->nins;
    return s;
}

static void ms_close(struct ms *m, struct ir_slice *s)
{
    s = &m->e.sl[s - m->e.sl];
    s->hi = m->e.fn->nins;
}

/* `dst = src` for the shadow, into the fixed shadow vreg of the def */
static void ms_set(struct ir_func *fn, int dst, int src)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_MOV;
    i->dst = dst;
    i->a = src;
}

static void ms_call(struct ms *m, const char *name, int n, int *v, int line)
{
    irgen_san_call(m->e.iu, m->e.fn, name, n, v, line);
}

static int ms_call_ret(struct ms *m, const char *name, int n, int *v, int line)
{
    return irgen_san_call_ret(m->e.iu, m->e.fn, name, n, v, line, 1);
}

/* is the function one this pass can follow */
static int ms_followable(const struct ir_func *fn)
{
    if (fn->has_i128)
        return 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->w == 16 || i->size == 16 ||
            (i->op >= IR_VLOAD && i->op <= IR_VWIDEN && i->op != IR_SELECT))
            return 0;
        if (i->op == IR_LOAD && i->size > 8)
            return 0;
    }
    return 1;
}

/* the shadow of x != 0, at width w: 0 or all ones */
/* (no pointer from emit() is held across another emit(): the array may
 * move) */
static int ms_cmpne0(struct ir_func *fn, int s, int w)
{
    int z = ms_const(fn, 0, w);
    struct ir_ins *c = emit(fn);
    c->op = IR_CMP;
    c->pred = B_NE;
    c->a = s;
    c->b = z;
    c->w = w;
    c->dst = ms_new(fn);
    return c->dst;
}

static int ms_smear(struct ir_func *fn, int s, int w)
{
    int ne = ms_cmpne0(fn, s, w);
    int z = ms_const(fn, 0, w);
    return ms_op(fn, IR_SUB, z, ne, w, 1);
}

static void ms_check(struct ms *m, int at, int v, int w, int line)
{
    int s = S(m, v);
    if (s < 0)
        return;
    struct ir_slice *sl = ms_open(m, at);
    int a = ms_arg(m->e.fn, s, w);
    ms_call(m, "__embcc_msan_check", 1, &a, line);
    ms_close(m, sl);
}

static void msan_func(struct ir_unit *iu, struct ir_func *fn)
{
    if (!ms_followable(fn))
        return;
    struct ms m;
    memset(&m, 0, sizeof m);
    m.e.iu = iu;
    m.e.fn = fn;
    int n0 = fn->nins, nv0 = fn->nvregs, P = target_ptr_size();
    m.nsh = nv0;
    m.sh = xmalloc((size_t)(nv0 ? nv0 : 1) * sizeof *m.sh);
    for (int v = 0; v < nv0; v++)
        m.sh[v] = -1;

    /* defs per vreg: one that is read and not defined exactly once may be
     * read where no def reached it (mem2reg's merges), and starts out
     * poisoned */
    int *ndef = xcalloc((size_t)(nv0 ? nv0 : 1), sizeof *ndef);
    char *read = xcalloc((size_t)(nv0 ? nv0 : 1), 1);
    char *slot = xcalloc((size_t)(fn->nvars ? fn->nvars : 1), 1);
    for (int n = 0; n < n0; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->dst >= 0 && i->dst < nv0) ndef[i->dst]++;
        if (i->op == IR_LANDING && i->b >= 0 && i->b < nv0) ndef[i->b]++;
        if (i->op == IR_LDVAR || i->op == IR_STVAR || i->op == IR_ADDR) {
            int v = i->op == IR_STVAR ? i->dst : i->a;
            if (v >= 0 && v < fn->nvars) slot[v] = 1;
        } else if (i->op != IR_CONST && i->op != IR_LABEL && i->op != IR_JMP) {
            if (i->a >= 0 && i->a < nv0) read[i->a] = 1;
            if (i->b >= 0 && i->b < nv0 && !i->imm_b) read[i->b] = 1;
            if ((i->op == IR_SELECT || i->op == IR_CAS ||
                 i->op == IR_CMPXCHG) && i->c >= 0 && i->c < nv0)
                read[i->c] = 1;
        }
        if (i->op == IR_STVAR && i->a >= 0 && i->a < nv0) read[i->a] = 1;
    }

    /* entry: the parameters' shadows into their slots, the locals' slots
     * poisoned, the undefinable vregs poisoned */
    {
        struct ir_slice *sl = ms_open(&m, 0);
        int line = n0 ? fn->ins[0].line : 0;
        /* a parameter is also vreg k, its incoming value, which nothing
         * defines: its shadow is the caller's, from the slot */
        int *pshadow = xcalloc((size_t)(fn->nparams ? fn->nparams : 1),
                               sizeof *pshadow);
        for (int k = 0; k < fn->nparams; k++) {
            pshadow[k] = -1;
            if (!fn->param_abi || fn->param_abi[k].is_struct ||
                (fn->locals && fn->locals[k].size > 8))
                continue;
            int kk = put_const(fn, k);
            pshadow[k] = ms_call_ret(&m, "__embcc_msan_get_param", 1, &kk,
                                     line);
            if (k < nv0)
                ms_set(fn, S(&m, k), pshadow[k]);
        }
        for (int v = 0; v < fn->nvars; v++) {
            if (!slot[v] || !fn->locals)
                continue;
            int size = fn->locals[v].size;
            if (size <= 0)
                continue;
            struct ir_ins *ad = emit(fn);
            ad->op = IR_ADDR;
            ad->a = v;
            ad->dst = ms_new(fn);
            ad->w = P;
            int av = ad->dst;
            int args[3] = { av, put_const(fn, size), -1 };
            if (v < fn->nparams) {
                if (pshadow[v] >= 0) {
                    args[2] = pshadow[v];
                    ms_call(&m, "__embcc_msan_store", 3, args, line);
                } else {
                    ms_call(&m, "__embcc_msan_unpoison", 2, args, line);
                }
            } else {
                ms_call(&m, "__embcc_msan_poison", 2, args, line);
            }
        }
        free(pshadow);
        for (int v = fn->nparams; v < nv0; v++)
            if (read[v] && ndef[v] != 1) {
                int s = S(&m, v);
                ms_set(fn, s, ms_const(fn, -1, 8));
            }
        ms_close(&m, sl);
    }

    for (int n = 0; n < n0; n++) {
        struct ir_ins i = fn->ins[n];          /* (emit may move them) */
        int line = i.line, w = i.w ? i.w : 4;
        struct ir_slice *sl;
        switch (i.op) {
        case IR_CONST: case IR_ADDR: case IR_STRADDR: case IR_GADDR:
        case IR_FADDR: case IR_LABELADDR: case IR_FRAMEADDR:
        case IR_SPSAVE:
            if (i.dst >= 0) {
                sl = ms_open(&m, n + 1);
                ms_set(fn, S(&m, i.dst), ms_const(fn, 0, 8));
                ms_close(&m, sl);
            }
            break;
        case IR_MOV:
            sl = ms_open(&m, n + 1);
            ms_set(fn, S(&m, i.dst), S(&m, i.a));
            ms_close(&m, sl);
            break;
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_XOR: case IR_MULH:
        case IR_MULW: case IR_DIV: case IR_MOD: {
            if ((i.op == IR_DIV || i.op == IR_MOD) && !i.imm_b)
                ms_check(&m, n, i.b, w, line);
            sl = ms_open(&m, n + 1);
            int s = i.imm_b ? S(&m, i.a)
                            : ms_op(fn, IR_OR, S(&m, i.a), S(&m, i.b), w, 0);
            if (i.op == IR_MUL || i.op == IR_DIV || i.op == IR_MOD ||
                i.op == IR_MULH || i.op == IR_MULW)
                s = ms_smear(fn, s, i.op == IR_MULW ? 4 : w);
            ms_set(fn, S(&m, i.dst), s);
            ms_close(&m, sl);
            break;
        }
        case IR_AND: case IR_OR: {
            /* AND: (Sa & Sb) | (Sa & b) | (a & Sb); OR the same with the
             * operands complemented */
            sl = ms_open(&m, n + 1);
            int sa = S(&m, i.a), r;
            if (i.imm_b) {
                long k = i.op == IR_AND ? i.imm : ~i.imm;
                r = ms_op(fn, IR_AND, sa, ms_const(fn, k, w), w, 0);
            } else {
                int sb = S(&m, i.b), a = i.a, b = i.b;
                if (i.op == IR_OR) {
                    a = ms_op(fn, IR_BNOT, a, -1, w, 0);
                    b = ms_op(fn, IR_BNOT, b, -1, w, 0);
                }
                int x = ms_op(fn, IR_AND, sa, sb, w, 0);
                int y = ms_op(fn, IR_AND, sa, b, w, 0);
                int z = ms_op(fn, IR_AND, a, sb, w, 0);
                r = ms_op(fn, IR_OR, ms_op(fn, IR_OR, x, y, w, 0), z, w, 0);
            }
            ms_set(fn, S(&m, i.dst), r);
            ms_close(&m, sl);
            break;
        }
        case IR_SHL: case IR_SHR: {
            sl = ms_open(&m, n + 1);
            int r;
            if (i.imm_b) {
                struct ir_ins *q = emit(fn);
                q->op = i.op;
                q->a = S(&m, i.a);
                q->imm = i.imm;
                q->imm_b = 1;
                q->w = w;
                q->sign = i.sign;
                q->dst = ms_new(fn);
                r = q->dst;
            } else {
                r = ms_op(fn, i.op, S(&m, i.a), i.b, w, i.sign);
                r = ms_op(fn, IR_OR, r, ms_smear(fn, S(&m, i.b), w), w, 0);
            }
            ms_set(fn, S(&m, i.dst), r);
            ms_close(&m, sl);
            break;
        }
        case IR_NEG: case IR_BNOT: case IR_BSWAP:
            sl = ms_open(&m, n + 1);
            if (i.op == IR_BSWAP) {
                struct ir_ins *q = emit(fn);
                *q = i;
                q->a = S(&m, i.a);
                q->dst = ms_new(fn);
                ms_set(fn, S(&m, i.dst), q->dst);
            } else {
                ms_set(fn, S(&m, i.dst), i.op == IR_NEG
                           ? ms_smear(fn, S(&m, i.a), w) : S(&m, i.a));
            }
            ms_close(&m, sl);
            break;
        case IR_CMP: {
            sl = ms_open(&m, n + 1);
            int s = i.imm_b ? S(&m, i.a)
                            : ms_op(fn, IR_OR, S(&m, i.a), S(&m, i.b), w, 0);
            int ne = ms_cmpne0(fn, s, w);
            ms_set(fn, S(&m, i.dst), ne);
            ms_close(&m, sl);
            break;
        }
        case IR_EXT: {
            sl = ms_open(&m, n + 1);
            struct ir_ins *q = emit(fn);
            q->op = IR_EXT;
            q->a = S(&m, i.a);
            q->size = i.size;
            q->sign = i.sign;
            q->w = i.w;
            q->dst = ms_new(fn);
            ms_set(fn, S(&m, i.dst), q->dst);
            ms_close(&m, sl);
            break;
        }
        case IR_BITCAST:
            sl = ms_open(&m, n + 1);
            ms_set(fn, S(&m, i.dst), S(&m, i.a));
            ms_close(&m, sl);
            break;
        case IR_I2F: case IR_F2I: case IR_F2F: case IR_SQRT: case IR_CLZ:
        case IR_CTZ: case IR_POPCNT:
            sl = ms_open(&m, n + 1);
            ms_set(fn, S(&m, i.dst),
                   ms_smear(fn, ms_arg(fn, S(&m, i.a),
                                       i.op == IR_I2F ? (i.size < 4 ? 4 : i.size)
                                                      : w), 8));
            ms_close(&m, sl);
            break;
        case IR_SELECT: {
            sl = ms_open(&m, n + 1);
            struct ir_ins *q = emit(fn);
            *q = i;
            q->a = i.a;
            q->b = S(&m, i.b);
            q->c = S(&m, i.c);
            q->flt = 0;
            q->dst = ms_new(fn);
            int sel = q->dst;
            int sm = ms_smear(fn, S(&m, i.a), i.size ? i.size : 4);
            int r = ms_op(fn, IR_OR, sel, sm, w, 0);
            ms_set(fn, S(&m, i.dst), r);
            ms_close(&m, sl);
            break;
        }
        case IR_LDVAR: case IR_LOAD: {
            if (i.op == IR_LOAD)
                ms_check(&m, n, i.a, P, line);
            sl = ms_open(&m, n + 1);
            int addr = i.a;
            if (i.op == IR_LDVAR) {
                struct ir_ins *ad = emit(fn);
                ad->op = IR_ADDR;
                ad->a = i.a;
                ad->w = P;
                ad->dst = ms_new(fn);
                addr = ad->dst;
            }
            int args[2] = { addr, put_const(fn, i.size) };
            int s = ms_call_ret(&m, "__embcc_msan_load", 2, args, line);
            if (i.size < 8) {
                struct ir_ins *q = emit(fn);
                q->op = IR_EXT;
                q->a = s;
                q->size = i.size;
                q->sign = i.sign;
                q->w = w;
                q->dst = ms_new(fn);
                s = q->dst;
            }
            ms_set(fn, S(&m, i.dst), s);
            ms_close(&m, sl);
            break;
        }
        case IR_STVAR: case IR_STORE: {
            if (i.op == IR_STORE)
                ms_check(&m, n, i.a, P, line);
            sl = ms_open(&m, n);
            int addr = i.a, val = i.op == IR_STVAR ? i.a : i.b;
            if (i.op == IR_STVAR) {
                struct ir_ins *ad = emit(fn);
                ad->op = IR_ADDR;
                ad->a = i.dst;
                ad->w = P;
                ad->dst = ms_new(fn);
                addr = ad->dst;
            }
            int vw = i.size > 4 ? 8 : 4;
            int args[3] = { addr, put_const(fn, i.size),
                            ms_arg(fn, S(&m, val), vw) };
            ms_call(&m, "__embcc_msan_store", 3, args, line);
            ms_close(&m, sl);
            break;
        }
        case IR_MEMCPY: case IR_MEMZERO: {
            sl = ms_open(&m, n);
            if (i.op == IR_MEMCPY) {
                int args[3] = { i.a, i.b, put_const(fn, i.size) };
                ms_call(&m, "__embcc_msan_copy", 3, args, line);
            } else {
                int args[2] = { i.a, put_const(fn, i.size) };
                ms_call(&m, "__embcc_msan_unpoison", 2, args, line);
            }
            ms_close(&m, sl);
            break;
        }
        case IR_ALLOCA:
            if (i.dst >= 0) {
                sl = ms_open(&m, n + 1);
                int args[2] = { i.dst, i.a };
                ms_call(&m, "__embcc_msan_poison", 2, args, line);
                ms_set(fn, S(&m, i.dst), ms_const(fn, 0, 8));
                ms_close(&m, sl);
            }
            break;
        case IR_BRZ: case IR_BRNZ: case IR_SWITCH: case IR_IGOTO:
            ms_check(&m, n, i.a, i.op == IR_SWITCH || i.op == IR_IGOTO
                                     ? (i.op == IR_IGOTO ? P : w) : w, line);
            break;
        case IR_RET: {
            sl = ms_open(&m, n);
            if (i.a >= 0) {
                int a = ms_arg(fn, S(&m, i.a), w);
                ms_call(&m, "__embcc_msan_retval", 1, &a, line);
            }
            /* the locals made good again: the next frame here may keep
             * saved registers or va_arg's data in the same bytes */
            for (int v = fn->nparams; v < fn->nvars; v++) {
                if (!slot[v] || !fn->locals || fn->locals[v].size <= 0)
                    continue;
                struct ir_ins *ad = emit(fn);
                ad->op = IR_ADDR;
                ad->a = v;
                ad->dst = ms_new(fn);
                ad->w = P;
                int av = ad->dst;
                int args[2] = { av, put_const(fn, fn->locals[v].size) };
                ms_call(&m, "__embcc_msan_unpoison", 2, args, line);
            }
            ms_close(&m, sl);
            break;
        }
        case IR_VA_START: {
            sl = ms_open(&m, n + 1);
            int args[2] = { i.a, put_const(fn, 32) };
            ms_call(&m, "__embcc_msan_unpoison", 2, args, line);
            ms_close(&m, sl);
            break;
        }
        case IR_CALL: {
            struct func *r = i.indirect ? NULL
                           : renamed(i.callee, "__embcc_msan_", msan_names);
            if (r) {
                fn->ins[n].callee = r;
                fn->ins[n].callee_sym = ir_sym_func(iu, r);
            }
            if (i.indirect)
                ms_check(&m, n, i.a, P, line);
            sl = ms_open(&m, n);
            for (int k = 0; k < i.nargs && k < 16; k++) {
                if (i.argv[k].is_struct)
                    continue;
                int sz = i.argv[k].size > 4 ? 8 : 4;
                int args[2] = { put_const(fn, k),
                                ms_arg(fn, S(&m, i.argv[k].vreg), sz) };
                ms_call(&m, "__embcc_msan_param", 2, args, line);
            }
            ms_close(&m, sl);
            if (i.dst >= 0) {
                sl = ms_open(&m, n + 1);
                int s = ms_call_ret(&m, "__embcc_msan_get_retval", 0, NULL, line);
                ms_set(fn, S(&m, i.dst), s);
                ms_close(&m, sl);
            }
            break;
        }
        case IR_ASM:
            sl = ms_open(&m, n + 1);
            if (i.dst >= 0)
                ms_set(fn, S(&m, i.dst), ms_const(fn, 0, 8));
            for (int k = 0; i.asm_ir && k < i.asm_ir->nout; k++) {
                const struct ir_asm_op *o = &i.asm_ir->out[k];
                if (o->temp >= 0 && !o->val) {
                    int args[2] = { o->temp, put_const(fn, o->size) };
                    ms_call(&m, "__embcc_msan_unpoison", 2, args, line);
                }
            }
            ms_close(&m, sl);
            break;
        case IR_LANDING:
            sl = ms_open(&m, n + 1);
            if (i.dst >= 0) ms_set(fn, S(&m, i.dst), ms_const(fn, 0, 8));
            if (i.b >= 0) ms_set(fn, S(&m, i.b), ms_const(fn, 0, 8));
            ms_close(&m, sl);
            break;
        default:
            /* the atomics and anything else: its value initialized */
            if (i.dst >= 0 && i.dst < nv0) {
                sl = ms_open(&m, n + 1);
                ms_set(fn, S(&m, i.dst), ms_const(fn, 0, 8));
                ms_close(&m, sl);
            }
            break;
        }
    }
    if (m.e.ns)
        ir_splice(fn, n0, fn->nins, m.e.sl, m.e.ns);
    free(m.e.sl);
    free(m.sh);
    free(ndef);
    free(read);
    free(slot);
}
