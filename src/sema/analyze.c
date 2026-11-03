/* The static analyzer (--analyze; GCC's -fanalyzer): what one translation
 * unit's code does that is wrong on some path, decided from its tree
 * after sema. Two halves.
 *
 * Interrupts. An embedded program's second thread of control is its
 * interrupt handlers, and their bugs are a compiler's to find: which
 * functions are handlers (the interrupt/signal attributes, AVR's
 * __vector_N, CMSIS's *_IRQHandler and *_Handler names, anything a vector
 * table -- a global in a section named for vectors -- points at), what
 * each runs (the unit's call graph), where the rest of the code turns
 * interrupts off (__disable_irq/__enable_irq, cli/sei, the RTOS's
 * critical-section calls, PRIMASK, an asm's cpsid/cpsie, AVR's
 * ATOMIC_BLOCK), and every access to an object both sides use. What is
 * reported: an object shared without volatile, a read-modify-write or
 * an access too wide to be one instruction outside a critical section,
 * a handler calling what may block or allocate, a function both sides
 * call that keeps static state, and a critical section left open.
 *
 * Memory. Within one function, per local pointer, along the statements:
 * allocated (unchecked or checked for NULL), freed, or escaped (passed
 * on, stored, returned). An if's arms are followed separately and joined
 * (a pointer freed on one arm only is no longer known), a loop's body
 * once and joined with what came before, so nothing is reported that
 * some path does not do. GCC's -Wanalyzer- names. */

#include "sema.h"
#include "type.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../driver/util.h"
#include "../arch/target.h"

static const char *const g_checks[] = {
#define CHECK(n, t) n,
#include "analyze.def"
#undef CHECK
};
static const char *const g_check_text[] = {
#define CHECK(n, t) t,
#include "analyze.def"
#undef CHECK
};
#define NCHECKS ((int)(sizeof g_checks / sizeof g_checks[0]))

int analyze_count(void) { return NCHECKS; }
const char *analyze_name(int i) { return g_checks[i]; }

static const char *g_file;
static struct unit *g_unit;

static void report(const char *name, int line, int col, const char *fmt, ...)
{
    if (!diag_warning_enabled(name))
        return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_warn_opt(g_file, line, col, name, "%s", msg);
}

static int any_on(void)
{
    for (int i = 0; i < NCHECKS; i++)
        if (diag_warning_enabled(g_checks[i]))
            return 1;
    return 0;
}

static const struct expr *strip(const struct expr *e)
{
    while (e && e->kind == EXPR_CAST && e->col == 0 && e->rhs)
        e = e->rhs;
    return e;
}

static const char *call_name(const struct expr *e)
{
    if (e->callee && e->callee->name)
        return e->callee->name;
    if (e->lhs && e->lhs->kind == EXPR_VAR)
        return e->lhs->name;
    return e->name;
}

static int name_in(const char *n, const char *const *l)
{
    for (int k = 0; n && l[k]; k++)
        if (!strcmp(n, l[k]))
            return 1;
    return 0;
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

/* ======================================================================
 * Interrupts
 * ====================================================================== */

struct fnode {
    struct func *f;
    int isr;                    /* a handler itself */
    struct func *by_isr;        /* a handler that runs it (or itself) */
    int thread;                 /* code that is not a handler runs it */
    int static_state;           /* writes a static object, unprotected */
    int all_calls_critical;     /* every call of it is in a critical section */
    int ncalls;
};

static struct fnode *g_fn;
static int g_nfn;

static struct fnode *node(const struct func *f)
{
    for (int i = 0; i < g_nfn; i++)
        if (g_fn[i].f == f)
            return &g_fn[i];
    return NULL;
}

struct edge { struct func *from, *to; int critical; };
static struct edge *g_edges;
static int g_nedges, g_capedges;

struct access {
    struct global *g;
    struct func *f;
    int write, rmw, size, critical, line, col;
};
static struct access *g_acc;
static int g_nacc, g_capacc;

static const char *const disable_fns[] = {
    "__disable_irq", "cli", "__cli", "noInterrupts", "portDISABLE_INTERRUPTS",
    "taskENTER_CRITICAL", "portENTER_CRITICAL", "vPortEnterCritical",
    "taskENTER_CRITICAL_FROM_ISR", "portSET_INTERRUPT_MASK_FROM_ISR",
    "irq_lock", "arch_irq_lock", "__disable_interrupt", "_disable_interrupts",
    "__disable_interrupts", "disable_interrupts", "DISABLE_INTERRUPTS",
    "ENTER_CRITICAL", "enter_critical", "k_spin_lock", "__iCliRetVal",
    "chSysLock", "chSysLockFromISR", "rt_hw_interrupt_disable",
    "__set_BASEPRI_MAX", "local_irq_disable", "local_irq_save", NULL
};
static const char *const enable_fns[] = {
    "__enable_irq", "sei", "__sei", "interrupts", "portENABLE_INTERRUPTS",
    "taskEXIT_CRITICAL", "portEXIT_CRITICAL", "vPortExitCritical",
    "taskEXIT_CRITICAL_FROM_ISR", "portCLEAR_INTERRUPT_MASK_FROM_ISR",
    "irq_unlock", "arch_irq_unlock", "__enable_interrupt",
    "_enable_interrupts", "__enable_interrupts", "enable_interrupts",
    "ENABLE_INTERRUPTS", "EXIT_CRITICAL", "exit_critical", "k_spin_unlock",
    "chSysUnlock", "chSysUnlockFromISR", "rt_hw_interrupt_enable",
    "local_irq_enable", "local_irq_restore", NULL
};
static const char *const unsafe_fns[] = {
    "malloc", "calloc", "realloc", "free", "aligned_alloc", "posix_memalign",
    "printf", "fprintf", "vprintf", "vfprintf", "puts", "fputs", "fputc",
    "fwrite", "fread", "fopen", "fclose", "fflush", "scanf", "fscanf",
    "getchar", "gets", "fgets", "perror", "exit", "sleep", "usleep",
    "nanosleep", "HAL_Delay", "delay", "vTaskDelay", "vTaskDelayUntil",
    "xQueueSend", "xQueueSendToBack", "xQueueSendToFront", "xQueueReceive",
    "xSemaphoreTake", "xSemaphoreGive", "xTaskCreate", "vTaskSuspend",
    "xEventGroupSetBits", "xTaskNotify", "xTaskNotifyGive",
    "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_cond_wait",
    "sem_wait", "mtx_lock", "k_sleep", "k_msleep", "k_mutex_lock",
    "osDelay", "osMutexAcquire", "osSemaphoreAcquire", NULL
};

static int asm_disables(const char *t)
{
    return t && (strstr(t, "cpsid") || strstr(t, "cli") ||
                 strstr(t, "csrci mstatus") || strstr(t, "csrc mstatus") ||
                 strstr(t, "di\n") || !strcmp(t, "di") || strstr(t, "rsil"));
}

static int asm_enables(const char *t)
{
    return t && (strstr(t, "cpsie") || strstr(t, "sei") ||
                 strstr(t, "csrsi mstatus") || strstr(t, "csrs mstatus") ||
                 strstr(t, "ei\n") || !strcmp(t, "ei"));
}

static struct global *global_of(const struct expr *e)
{
    return e && e->kind == EXPR_VAR ? e->gref : NULL;
}

/* the object an lvalue names, and the bytes one access of it moves */
static struct global *lvalue_global(const struct expr *e, int *size)
{
    e = strip(e);
    if (!e)
        return NULL;
    *size = e->ty ? ty_size(e->ty) : 0;
    while (e && ((e->kind == EXPR_MEMBER && !e->is_arrow) ||
                 e->kind == EXPR_DEREF)) {
        if (e->kind == EXPR_DEREF) {
            const struct expr *b = strip(e->rhs);
            if (b && b->kind == EXPR_BINOP && b->is_subscript)
                e = strip(b->lhs);
            else
                return NULL;
        } else {
            e = strip(e->lhs);
        }
    }
    return global_of(e);
}

static struct func *g_cur;
static int g_crit;

static void add_access(struct global *g, int write, int rmw, int size,
                       const struct expr *at)
{
    if (!g || g->is_const)
        return;
    if (g_nacc == g_capacc) {
        g_capacc = g_capacc ? g_capacc * 2 : 256;
        g_acc = xrealloc(g_acc, (size_t)g_capacc * sizeof *g_acc);
    }
    struct access *a = &g_acc[g_nacc++];
    a->g = g;
    a->f = g_cur;
    a->write = write;
    a->rmw = rmw;
    a->size = size;
    a->critical = g_crit > 0;
    a->line = at->line;
    a->col = at->col;
}

static int reads_global(const struct expr *e, const struct global *g)
{
    if (!e)
        return 0;
    if (e->kind == EXPR_VAR && global_of(e) == g)
        return 1;
    if (reads_global(e->lhs, g) || reads_global(e->rhs, g))
        return 1;
    for (int k = 0; e->args && k < e->nargs; k++)
        if (reads_global(e->args[k], g))
            return 1;
    return 0;
}

static void isr_expr(struct expr *e);

static void isr_call(struct expr *e)
{
    const char *n = call_name(e);
    if (n && name_in(n, disable_fns))
        g_crit++;
    else if (n && name_in(n, enable_fns) && g_crit > 0)
        g_crit--;
    else if (n && !strcmp(n, "__set_PRIMASK") && e->nargs == 1) {
        long v;
        if (sema_const_fold(e->args[0], &v) && v)
            g_crit++;
        else if (g_crit > 0)
            g_crit--;
    }
    if (e->callee && e->callee->has_defn) {
        if (g_nedges == g_capedges) {
            g_capedges = g_capedges ? g_capedges * 2 : 128;
            g_edges = xrealloc(g_edges, (size_t)g_capedges * sizeof *g_edges);
        }
        g_edges[g_nedges].from = g_cur;
        g_edges[g_nedges].to = e->callee;
        g_edges[g_nedges].critical = g_crit > 0;
        g_nedges++;
    }
    for (int k = 0; k < e->nargs; k++)
        isr_expr(e->args[k]);
}

static void isr_expr(struct expr *e)
{
    if (!e)
        return;
    int size;
    struct global *g;
    switch (e->kind) {
    case EXPR_ASSIGN:
        g = lvalue_global(e->lhs, &size);
        if (g)
            add_access(g, 1, reads_global(e->rhs, g), size, e);
        else
            isr_expr(e->lhs);
        isr_expr(e->rhs);
        return;
    case EXPR_COMPOUND: case EXPR_INCDEC:
        g = lvalue_global(e->lhs, &size);
        if (g)
            add_access(g, 1, 1, size, e);
        else
            isr_expr(e->lhs);
        isr_expr(e->rhs);
        return;
    case EXPR_ADDR:
        return;                 /* the object's address: not an access */
    case EXPR_CALL:
        isr_call(e);
        return;
    case EXPR_VAR: case EXPR_MEMBER: case EXPR_DEREF:
        g = lvalue_global(e, &size);
        if (g) {
            add_access(g, 0, 0, size, e);
            if (e->kind == EXPR_DEREF) {
                const struct expr *b = strip(e->rhs);
                if (b && b->kind == EXPR_BINOP)
                    isr_expr(b->rhs);   /* the index */
            }
            return;
        }
        break;
    case EXPR_STMTEXPR:
        return;
    default:
        break;
    }
    isr_expr(e->lhs);
    isr_expr(e->rhs);
    for (int k = 0; e->args && k < e->nargs; k++)
        isr_expr(e->args[k]);
    for (int k = 0; e->inits && k < e->ninits; k++)
        isr_expr(e->inits[k].e);
}

static int init_disables(const struct expr *e)
{
    if (!e)
        return 0;
    if (e->kind == EXPR_CALL && call_name(e) &&
        name_in(call_name(e), disable_fns))
        return 1;
    return init_disables(e->lhs) || init_disables(e->rhs);
}

static int stmt_disables(const struct stmt *s)
{
    if (!s)
        return 0;
    if (s->kind == STMT_DECL)
        for (; s && s->kind == STMT_DECL; s = s->next) {
            if (init_disables(s->expr))
                return 1;
            for (int k = 0; s->inits && k < s->ninits; k++)
                if (init_disables(s->inits[k].e))
                    return 1;
        }
    return 0;
}

static void isr_stmt(struct stmt *s)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_ASM:
            if (s->asm_s && asm_disables(s->asm_s->tmpl))
                g_crit++;
            else if (s->asm_s && asm_enables(s->asm_s->tmpl) && g_crit > 0)
                g_crit--;
            break;
        case STMT_FOR: {
            /* avr-libc's ATOMIC_BLOCK: interrupts off for the body, and
             * restored by the cleanup on the way out */
            int atomic = init_disables(s->init) || stmt_disables(s->initdecl);
            int saved = g_crit;
            isr_stmt(s->initdecl);
            isr_expr(s->init);
            isr_expr(s->cond);
            isr_stmt(s->body);
            isr_expr(s->step);
            if (atomic)
                g_crit = saved;
            break;
        }
        case STMT_IF: {
            isr_expr(s->cond);
            int c0 = g_crit;
            isr_stmt(s->thn);
            int c1 = g_crit;
            g_crit = c0;
            if (s->els) {
                struct stmt *e = s->els, *nx = e->next;
                e->next = NULL;
                isr_stmt(e);
                e->next = nx;
            }
            if (g_crit < c1)
                g_crit = c1;
            break;
        }
        default:
            isr_expr(s->expr);
            isr_expr(s->cond);
            isr_expr(s->init);
            isr_expr(s->step);
            for (int k = 0; s->inits && k < s->ninits; k++)
                isr_expr(s->inits[k].e);
            isr_stmt(s->initdecl);
            isr_stmt(s->thn);
            isr_stmt(s->els);
            isr_stmt(s->body);
            break;
        }
    }
}

static int is_handler_name(const char *n)
{
    return n && (ends_with(n, "_IRQHandler") || ends_with(n, "_Handler") ||
                 !strncmp(n, "__vector_", 9) || ends_with(n, "_isr") ||
                 ends_with(n, "_ISR"));
}

/* the machine's single-access width: what one load or store moves */
static int atomic_width(void)
{
    if (target_get() == TARGET_AVR)
        return 1;
    return target_ptr_size();
}

static void analyze_isr(struct unit *u)
{
    g_nfn = g_nedges = g_nacc = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->body)
            g_nfn++;
    g_fn = xcalloc((size_t)(g_nfn ? g_nfn : 1), sizeof *g_fn);
    int n = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->body) {
            g_fn[n].f = f;
            g_fn[n].isr = f->is_isr || is_handler_name(f->name);
            g_fn[n].all_calls_critical = 1;
            n++;
        }
    /* a vector table's entries are handlers */
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->section &&
            (strstr(g->section, "vector") || strstr(g->section, "isr")))
            for (int r = 0; r < g->nrelocs; r++)
                if (g->relocs[r].ftarget) {
                    struct fnode *fn = node(g->relocs[r].ftarget);
                    if (fn && strcmp(fn->f->name, "main") &&
                        !ends_with(fn->f->name, "Reset_Handler"))
                        fn->isr = 1;
                }
    for (int i = 0; i < g_nfn; i++)
        if (ends_with(g_fn[i].f->name, "Reset_Handler"))
            g_fn[i].isr = 0;           /* the startup code, not a handler */
    int any_isr = 0;
    for (int i = 0; i < g_nfn; i++)
        any_isr |= g_fn[i].isr;
    /* the accesses and the calls, with where interrupts are off */
    for (int i = 0; i < g_nfn; i++) {
        g_cur = g_fn[i].f;
        g_crit = 0;
        isr_stmt(g_cur->body);
        if (g_crit > 0 && !g_fn[i].isr) {
            g_file = g_cur->file ? g_cur->file : u->file;
            report("isr-critical-unbalanced", g_cur->line, 0,
                   "'%s' leaves a critical section open: interrupts are "
                   "still disabled where it returns", g_cur->name);
        }
    }
    if (!any_isr)
        goto done;
    /* what runs in a handler, and what in the code it interrupts */
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int i = 0; i < g_nfn; i++)
            if (g_fn[i].isr && !g_fn[i].by_isr) {
                g_fn[i].by_isr = g_fn[i].f;
                changed = 1;
            }
        for (int k = 0; k < g_nedges; k++) {
            struct fnode *a = node(g_edges[k].from), *b = node(g_edges[k].to);
            if (a && b && a->by_isr && !b->by_isr) {
                b->by_isr = a->by_isr;
                changed = 1;
            }
        }
    }
    for (int i = 0; i < g_nfn; i++)
        g_fn[i].thread = !g_fn[i].isr &&
                         (!strcmp(g_fn[i].f->name, "main") ||
                          !g_fn[i].f->is_static || g_fn[i].f->is_ctor);
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int k = 0; k < g_nedges; k++) {
            struct fnode *a = node(g_edges[k].from), *b = node(g_edges[k].to);
            if (a && b && a->thread && !b->thread && !b->isr) {
                b->thread = 1;
                changed = 1;
            }
        }
    }
    /* a static function called only inside critical sections is in one */
    for (int k = 0; k < g_nedges; k++) {
        struct fnode *a = node(g_edges[k].from), *b = node(g_edges[k].to);
        if (!a || !b || !a->thread)
            continue;
        b->ncalls++;
        if (!g_edges[k].critical)
            b->all_calls_critical = 0;
    }
    int aw = atomic_width();
    for (int i = 0; i < g_nacc; i++) {
        struct access *a = &g_acc[i];
        /* once per object, function and kind of finding */
        int dup = 0;
        for (int j = 0; j < i && !dup; j++)
            dup = g_acc[j].g == a->g && g_acc[j].f == a->f &&
                  g_acc[j].write == a->write && g_acc[j].rmw == a->rmw &&
                  !g_acc[j].critical;
        if (dup)
            continue;
        struct fnode *fa = node(a->f);
        if (!fa || !fa->thread || a->critical ||
            (fa->f->is_static && fa->ncalls && fa->all_calls_critical))
            continue;
        /* what the handlers do to it */
        int isr_w = 0, isr_r = 0;
        struct func *h = NULL;
        for (int j = 0; j < g_nacc; j++) {
            struct fnode *fb = node(g_acc[j].f);
            if (g_acc[j].g != a->g || !fb || !fb->by_isr)
                continue;
            if (g_acc[j].write) { isr_w = 1; h = fb->by_isr; }
            else { isr_r = 1; if (!h) h = fb->by_isr; }
        }
        if (!h)
            continue;
        g_file = a->f->file ? a->f->file : u->file;
        int vol = a->g->ty && a->g->ty->is_volatile;
        if (!vol && ((isr_w && !a->write) || (a->write && isr_r)))
            report("isr-shared-nonvolatile", a->line, a->col,
                   "'%s' is %s by interrupt handler '%s' and %s here, and is "
                   "not volatile: the compiler may keep it in a register",
                   a->g->name, isr_w ? "written" : "read", h->name,
                   a->write ? "written" : "read");
        if (a->rmw && isr_w)
            report("isr-race-rmw", a->line, a->col,
                   "read-modify-write of '%s', which interrupt handler '%s' "
                   "also writes, outside a critical section: an interrupt "
                   "between the read and the write loses its update",
                   a->g->name, h->name);
        if (a->size > aw && ((a->write && (isr_r || isr_w)) ||
                             (!a->write && isr_w)))
            report("isr-torn-access", a->line, a->col,
                   "%s of '%s' (%d bytes; this machine moves %d at once) "
                   "while interrupt handler '%s' %s it: it can see half of "
                   "an update", a->write ? "a write" : "a read", a->g->name,
                   a->size, aw, h->name, isr_w ? "writes" : "reads");
    }
    /* a handler's calls, and what both sides call */
    for (int k = 0; k < g_nedges; k++)
        (void)k;
    for (int i = 0; i < g_nfn; i++) {
        struct fnode *fn = &g_fn[i];
        if (!fn->by_isr)
            continue;
        g_file = fn->f->file ? fn->f->file : u->file;
        /* the unsafe calls it makes (to functions not defined here) */
        struct stmt *body = fn->f->body;
        (void)body;
        if (fn->thread && !fn->isr) {
            for (int j = 0; j < g_nacc; j++)
                if (g_acc[j].f == fn->f && g_acc[j].write &&
                    !g_acc[j].critical) {
                    report("isr-reentrancy", fn->f->line, 0,
                           "'%s' is called by interrupt handler '%s' and by "
                           "the code it interrupts, and writes '%s': an "
                           "interrupt in the middle of it corrupts that state",
                           fn->f->name, fn->by_isr->name, g_acc[j].g->name);
                    break;
                }
        }
    }
done:
    free(g_fn);
    g_fn = NULL;
}

/* the unsafe calls a handler's code makes: a second walk, for the names */
static struct func *g_in_isr;
static void unsafe_expr(const struct expr *e)
{
    if (!e)
        return;
    if (e->kind == EXPR_CALL) {
        const char *n = call_name(e);
        if (n && name_in(n, unsafe_fns) && !(e->callee && e->callee->has_defn))
            report("isr-unsafe-call", e->line, e->col,
                   "'%s', which interrupt handler '%s' runs, calls '%s': it "
                   "may allocate, lock, block or do stdio, which is not safe "
                   "in an interrupt", g_cur->name, g_in_isr->name, n);
    }
    unsafe_expr(e->lhs);
    unsafe_expr(e->rhs);
    for (int k = 0; e->args && k < e->nargs; k++)
        unsafe_expr(e->args[k]);
}

static void unsafe_stmt(const struct stmt *s)
{
    for (; s; s = s->next) {
        unsafe_expr(s->expr);
        unsafe_expr(s->cond);
        unsafe_expr(s->init);
        unsafe_expr(s->step);
        for (int k = 0; s->inits && k < s->ninits; k++)
            unsafe_expr(s->inits[k].e);
        unsafe_stmt(s->initdecl);
        unsafe_stmt(s->thn);
        unsafe_stmt(s->els);
        unsafe_stmt(s->body);
    }
}

static void analyze_unsafe(struct unit *u)
{
    /* the handlers again, and what each reaches */
    int nf = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->body)
            nf++;
    struct func **fs = xcalloc((size_t)(nf ? nf : 1), sizeof *fs);
    struct func **by = xcalloc((size_t)(nf ? nf : 1), sizeof *by);
    int k = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->body) {
            fs[k] = f;
            if ((f->is_isr || is_handler_name(f->name)) &&
                !ends_with(f->name, "Reset_Handler"))
                by[k] = f;
            k++;
        }
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int e = 0; e < g_nedges; e++) {
            int a = -1, b = -1;
            for (int i = 0; i < nf; i++) {
                if (fs[i] == g_edges[e].from) a = i;
                if (fs[i] == g_edges[e].to) b = i;
            }
            if (a >= 0 && b >= 0 && by[a] && !by[b]) {
                by[b] = by[a];
                changed = 1;
            }
        }
    }
    for (int i = 0; i < nf; i++)
        if (by[i]) {
            g_cur = fs[i];
            g_in_isr = by[i];
            g_file = fs[i]->file ? fs[i]->file : u->file;
            unsafe_stmt(fs[i]->body);
        }
    free(fs);
    free(by);
}

/* ======================================================================
 * Memory
 * ====================================================================== */

enum mst { M_NONE, M_ALLOC, M_CHECKED, M_FREED, M_ESCAPED, M_UNKNOWN };

#define MAXV 64
struct mstate {
    int var[MAXV];
    unsigned char st[MAXV];
    int line[MAXV];
    int n;
};

static int ms_find(struct mstate *m, int v)
{
    for (int k = 0; k < m->n; k++)
        if (m->var[k] == v)
            return k;
    return -1;
}

static void ms_set(struct mstate *m, int v, int st, int line)
{
    int k = ms_find(m, v);
    if (k < 0) {
        if (m->n == MAXV)
            return;
        k = m->n++;
        m->var[k] = v;
    }
    m->st[k] = (unsigned char)st;
    m->line[k] = line;
}

static int ms_get(struct mstate *m, int v)
{
    int k = ms_find(m, v);
    return k < 0 ? M_NONE : m->st[k];
}

static void ms_join(struct mstate *a, const struct mstate *b)
{
    for (int k = 0; k < a->n; k++) {
        int j = -1;
        for (int q = 0; q < b->n; q++)
            if (b->var[q] == a->var[k]) j = q;
        int sb = j < 0 ? M_NONE : b->st[j];
        if (sb != a->st[k]) {
            /* allocated on one path, checked on the other: allocated */
            if ((a->st[k] == M_ALLOC && sb == M_CHECKED) ||
                (a->st[k] == M_CHECKED && sb == M_ALLOC))
                a->st[k] = M_ALLOC;
            else
                a->st[k] = M_UNKNOWN;
        }
    }
    for (int q = 0; q < b->n; q++)
        if (ms_find(a, b->var[q]) < 0 && b->st[q] != M_NONE)
            ms_set(a, b->var[q], M_UNKNOWN, b->line[q]);
}

static const char *const alloc_fns[] = { "malloc", "calloc", "realloc",
    "strdup", "strndup", "aligned_alloc", NULL };

static int is_alloc_call(const struct expr *e)
{
    e = strip(e);
    return e && e->kind == EXPR_CALL && call_name(e) &&
           name_in(call_name(e), alloc_fns);
}

static int local_var(const struct expr *e)
{
    e = strip(e);
    return e && e->kind == EXPR_VAR && !e->gref && e->var_index >= 0
           ? e->var_index : -1;
}

static struct func *g_mfn;

static void mem_expr(struct expr *e, struct mstate *m, int ctx);

/* a pointer dereferenced (*p, p->x, p[i]) */
static void deref_of(const struct expr *base, struct mstate *m,
                     const struct expr *at)
{
    const struct expr *b = strip(base);
    if (b && b->kind == EXPR_NUM && b->num == 0) {
        report("analyzer-null-dereference", at->line, at->col,
               "dereference of NULL");
        return;
    }
    if (b && b->kind == EXPR_CAST && b->col != 0) {
        const struct expr *c = strip(b->rhs);
        if (c && c->kind == EXPR_NUM && c->num == 0) {
            report("analyzer-null-dereference", at->line, at->col,
                   "dereference of NULL");
            return;
        }
    }
    int v = local_var(b);
    if (v < 0)
        return;
    int st = ms_get(m, v);
    const char *n = strip(b)->name ? strip(b)->name : "?";
    if (st == M_FREED)
        report("analyzer-use-after-free", at->line, at->col,
               "use of '%s' after it was freed (line %d)", n,
               m->line[ms_find(m, v)]);
    else if (st == M_ALLOC) {
        report("analyzer-possible-null-dereference", at->line, at->col,
               "'%s' may be NULL here: the allocation (line %d) was not "
               "checked", n, m->line[ms_find(m, v)]);
        ms_set(m, v, M_CHECKED, m->line[ms_find(m, v)]);
    }
}

/* What a condition says on the arm where it is `truth`: a pointer tested
 * is not NULL there (checked), or is NULL (and so holds nothing to leak) */
/* `(p = malloc(n))` in a condition tests p */
static const struct expr *assigned_var(const struct expr *c)
{
    c = strip(c);
    if (c && c->kind == EXPR_ASSIGN && local_var(c->lhs) >= 0)
        return strip(c->lhs);
    return c;
}

static void mem_cond(const struct expr *c, struct mstate *m, int truth)
{
    c = assigned_var(c);
    if (!c)
        return;
    int v = -1, nonnull = truth;
    if ((v = local_var(c)) >= 0)
        nonnull = truth;
    else if (c->kind == EXPR_NOT && (v = local_var(assigned_var(c->rhs))) >= 0)
        nonnull = !truth;
    else if (c->kind == EXPR_BINOP && (c->op == B_EQ || c->op == B_NE)) {
        const struct expr *l = assigned_var(c->lhs), *r = assigned_var(c->rhs);
        long z;
        int lv = local_var(l), rv = local_var(r);
        if (lv >= 0 && sema_const_fold(r, &z) && z == 0)
            v = lv;
        else if (rv >= 0 && sema_const_fold(l, &z) && z == 0)
            v = rv;
        nonnull = c->op == B_NE ? truth : !truth;
    } else if (c->kind == EXPR_BINOP && c->op == B_LAND && truth) {
        mem_cond(c->lhs, m, 1);
        mem_cond(c->rhs, m, 1);
        return;
    } else if (c->kind == EXPR_BINOP && c->op == B_LOR && !truth) {
        mem_cond(c->lhs, m, 0);
        mem_cond(c->rhs, m, 0);
        return;
    }
    if (v < 0)
        return;
    int st = ms_get(m, v);
    if (st == M_ALLOC || st == M_CHECKED)
        ms_set(m, v, nonnull ? M_CHECKED : M_NONE, m->line[ms_find(m, v)]);
}

/* ctx 1: the value goes somewhere (escapes) */
static void mem_expr(struct expr *e, struct mstate *m, int ctx)
{
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_ASSIGN: {
        int v = local_var(e->lhs);
        if (v >= 0 && is_alloc_call(e->rhs)) {
            const struct expr *c = strip(e->rhs);
            for (int k = 0; k < c->nargs; k++)
                mem_expr(c->args[k], m, 1);
            int st = ms_get(m, v);
            if ((st == M_ALLOC || st == M_CHECKED) &&
                strcmp(call_name(c), "realloc"))
                report("analyzer-malloc-leak", e->line, e->col,
                       "'%s' is given new memory and the memory it held "
                       "(line %d) is lost", e->lhs->name ? e->lhs->name : "?",
                       m->line[ms_find(m, v)]);
            ms_set(m, v, M_ALLOC, e->line);
            return;
        }
        if (v >= 0) {
            mem_expr(e->rhs, m, 1);
            ms_set(m, v, M_NONE, e->line);
            return;
        }
        /* *p = ..., p->x = ...: a dereference of p */
        const struct expr *l = strip(e->lhs);
        if (l && l->kind == EXPR_DEREF)
            deref_of(strip(l->rhs)->kind == EXPR_BINOP ? strip(l->rhs)->lhs
                                                        : l->rhs, m, e);
        else if (l && l->kind == EXPR_MEMBER && l->is_arrow)
            deref_of(l->lhs, m, e);
        mem_expr(e->rhs, m, 1);
        return;
    }
    case EXPR_CALL: {
        const char *n = call_name(e);
        if (n && !strcmp(n, "free") && e->nargs == 1) {
            int v = local_var(e->args[0]);
            if (v >= 0) {
                int st = ms_get(m, v);
                if (st == M_FREED)
                    report("analyzer-double-free", e->line, e->col,
                           "'%s' freed again (it was freed on line %d)",
                           strip(e->args[0])->name, m->line[ms_find(m, v)]);
                ms_set(m, v, M_FREED, e->line);
                return;
            }
        }
        for (int k = 0; k < e->nargs; k++) {
            int v = local_var(e->args[k]);
            if (v >= 0) {
                if (ms_get(m, v) == M_FREED)
                    report("analyzer-use-after-free", e->args[k]->line,
                           e->args[k]->col, "'%s' passed to '%s' after it "
                           "was freed (line %d)", strip(e->args[k])->name,
                           n ? n : "a function", m->line[ms_find(m, v)]);
                else if (ms_get(m, v) == M_ALLOC || ms_get(m, v) == M_CHECKED)
                    ms_set(m, v, M_ESCAPED, e->line);
            } else {
                mem_expr(e->args[k], m, 1);
            }
        }
        mem_expr(e->lhs, m, 0);
        return;
    }
    case EXPR_DEREF: {
        const struct expr *b = strip(e->rhs);
        if (b && b->kind == EXPR_BINOP && b->is_subscript) {
            deref_of(b->lhs, m, e);
            mem_expr(b->rhs, m, 0);
        } else {
            deref_of(e->rhs, m, e);
            mem_expr(e->rhs, m, 0);
        }
        return;
    }
    case EXPR_MEMBER:
        if (e->is_arrow)
            deref_of(e->lhs, m, e);
        mem_expr(e->lhs, m, 0);
        return;
    case EXPR_VAR: {
        int v = e->var_index;
        if (v >= 0 && ctx && (ms_get(m, v) == M_ALLOC || ms_get(m, v) == M_CHECKED))
            ms_set(m, v, M_ESCAPED, e->line);
        if (v >= 0 && ctx && ms_get(m, v) == M_FREED)
            ms_set(m, v, M_UNKNOWN, e->line);   /* stored: not followed */
        return;
    }
    case EXPR_BINOP:
        if (e->op == B_LAND || e->op == B_LOR) {
            /* the right operand runs only where the left is true (&&) or
             * false (||): `!b || b->x` reads b->x with b checked */
            mem_expr(e->lhs, m, 0);
            mem_cond(e->lhs, m, e->op == B_LAND);
            mem_expr(e->rhs, m, 0);
            return;
        }
        if (e->op == B_EQ || e->op == B_NE || e->op == B_LT ||
            e->op == B_GT || e->op == B_LE || e->op == B_GE) {
            mem_expr(e->lhs, m, 0);
            mem_expr(e->rhs, m, 0);
            return;
        }
        mem_expr(e->lhs, m, ctx);
        mem_expr(e->rhs, m, ctx);
        return;
    case EXPR_CAST:
        mem_expr(e->rhs, m, ctx);
        return;
    case EXPR_STMTEXPR:
        return;
    default:
        break;
    }
    mem_expr(e->lhs, m, ctx);
    mem_expr(e->rhs, m, ctx);
    for (int k = 0; e->args && k < e->nargs; k++)
        mem_expr(e->args[k], m, 1);
    for (int k = 0; e->inits && k < e->ninits; k++)
        mem_expr(e->inits[k].e, m, 1);
}

static void leaks_at(struct mstate *m, int line, int returned)
{
    for (int k = 0; k < m->n; k++)
        if ((m->st[k] == M_ALLOC || m->st[k] == M_CHECKED) &&
            m->var[k] != returned && g_mfn && m->var[k] < g_mfn->nvars &&
            m->var[k] >= g_mfn->nparams)
            report("analyzer-malloc-leak", line, 0,
                   "the memory allocated on line %d is lost here: nothing "
                   "points to it when the function returns", m->line[k]);
}

static int mem_stmt(struct stmt *s, struct mstate *m);

/* 1 when the statement list leaves the function on every path */
static int mem_stmt(struct stmt *s, struct mstate *m)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_DECL:
            if (s->expr && s->var_index >= 0 && is_alloc_call(s->expr)) {
                const struct expr *c = strip(s->expr);
                for (int k = 0; k < c->nargs; k++)
                    mem_expr(c->args[k], m, 1);
                ms_set(m, s->var_index, M_ALLOC, s->line);
            } else {
                mem_expr(s->expr, m, 1);
            }
            for (int k = 0; s->inits && k < s->ninits; k++)
                mem_expr(s->inits[k].e, m, 1);
            break;
        case STMT_EXPR:
            mem_expr(s->expr, m, 0);
            break;
        case STMT_RETURN: {
            int rv = local_var(s->expr);
            if (rv >= 0 && ms_get(m, rv) == M_FREED)
                report("analyzer-use-after-free", s->line, s->col,
                       "'%s' returned after it was freed",
                       strip(s->expr)->name);
            mem_expr(s->expr, m, 1);
            leaks_at(m, s->line, rv);
            return 1;
        }
        case STMT_IF: {
            mem_expr(s->cond, m, 0);
            struct mstate a = *m, b = *m;
            mem_cond(s->cond, &a, 1);
            mem_cond(s->cond, &b, 0);
            int ra = mem_stmt(s->thn, &a);
            int rb = 0;
            if (s->els) {
                struct stmt *e = s->els, *nx = e->next;
                e->next = NULL;
                rb = mem_stmt(e, &b);
                e->next = nx;
            }
            if (ra && rb)
                return 1;
            if (ra)
                *m = b;
            else if (rb)
                *m = a;
            else {
                *m = a;
                ms_join(m, &b);
            }
            break;
        }
        case STMT_WHILE: case STMT_FOR: case STMT_DO: {
            struct mstate before = *m;
            mem_stmt(s->initdecl, m);
            mem_expr(s->init, m, 0);
            mem_expr(s->cond, m, 0);
            struct mstate in = *m;
            (void)mem_stmt(s->body, &in);
            mem_expr(s->step, &in, 0);
            ms_join(m, &in);
            (void)before;
            break;
        }
        case STMT_SWITCH: {
            mem_expr(s->cond, m, 0);
            struct mstate in = *m;
            (void)mem_stmt(s->body, &in);
            ms_join(m, &in);
            break;
        }
        case STMT_BLOCK:
            if (mem_stmt(s->body, m))
                return 1;
            break;
        case STMT_LABEL: case STMT_GOTO: case STMT_CASE: case STMT_DEFAULT:
            /* a join from elsewhere: everything unknown from here */
            for (int k = 0; k < m->n; k++)
                if (m->st[k] != M_NONE)
                    m->st[k] = M_UNKNOWN;
            if (s->kind == STMT_LABEL && mem_stmt(s->body, m))
                return 1;
            break;
        default:
            mem_expr(s->expr, m, 0);
            break;
        }
    }
    return 0;
}

static void analyze_memory(struct unit *u)
{
    for (struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed || !f->body)
            continue;
        g_mfn = f;
        g_file = f->file ? f->file : u->file;
        struct mstate m;
        memset(&m, 0, sizeof m);
        int left = mem_stmt(f->body, &m);
        if (!left) {
            /* the closing brace */
            int last = f->line;
            for (const struct stmt *s = f->body->kind == STMT_BLOCK
                                        ? f->body->body : f->body;
                 s; s = s->next)
                last = s->line;
            leaks_at(&m, last, -1);
        }
    }
    g_mfn = NULL;
}

void analyze_unit(struct unit *u)
{
    if (!any_on())
        return;
    g_unit = u;
    analyze_isr(u);
    analyze_unsafe(u);
    analyze_memory(u);
    free(g_edges);
    free(g_acc);
    g_edges = NULL;
    g_acc = NULL;
    g_nedges = g_capedges = g_nacc = g_capacc = 0;
    (void)g_check_text;
}

const char *analyze_text(int i) { return g_check_text[i]; }
