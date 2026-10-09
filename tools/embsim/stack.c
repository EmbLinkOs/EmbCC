/* stack.c -- --stack-report and --stack-limit: how deep the stacks went,
 * by whom, and whether one went where it must not.
 *
 * After each step an_step gives stk_step the stack pointers: the main
 * stack's and, on a Cortex-M, the process stack's (each read as the
 * core banks it, so a thread on the PSP is measured on the PSP however
 * often handlers run on the MSP). Each one's high-water mark is its
 * lowest value; its top, the highest (on a Cortex-M the MSP's top is
 * also the reset value, from the vector table). The main stack's region
 * runs down from the top to its limit: --stack-limit, else the link's
 * __stack_limit (or __StackLimit, _sstack), else the end of .data and
 * .bss (_end, __bss_end, _ebss, end) when it lies below the top, else
 * the bottom of the RAM the stack is in. The main stack going below the
 * limit -- into .bss and .data, without one -- is an overflow: the run
 * ends there, as a part with a stack guard would fault, and the report
 * names the instruction that did it.
 *
 * By function, from the shadow stack's frames (analysis.c):
 *   - deepest: the lowest sp while the pc was in the function;
 *   - frame: the most it moved sp below its entry, which is what
 *     -fstack-usage's .su file says it uses. The entry is sp before the
 *     call: on the AVR the call's return address (two bytes) is the
 *     callee's, as avr-gcc's and EmbCC's .su count it, and an
 *     interrupt's too;
 *   - incl: the most the stack went below its entry while it was on the
 *     stack, its callees included and its interrupt handlers not, which
 *     is what embrt bounds for an entry point.
 * The .su files (--stack-su, or any argument ending in .su) and embrt's
 * JSON (--stack-embrt) are read to put their numbers beside these: a
 * measured frame or depth above the static one is marked, as the static
 * analysis is then wrong. */
#include <stdlib.h>
#include <string.h>

#include "analysis.h"

/* what a call pushes besides the callee's frame: the AVR's return
 * address (the ATmega328P's 16-bit program counter) */
#define RET_BYTES(a) ((a)->arch == AN_AVR ? 2u : 0u)

static const char *const limit_syms[] = { "__stack_limit", "__StackLimit",
                                          "_sstack", 0 };
static const char *const end_syms[] = { "_end", "__bss_end", "__bss_end__",
                                        "_ebss", "end", "__end__", 0 };

void stk_init(struct analysis *a)
{
    int n = a->img->nfn + 1;
    a->fn_minsp = malloc((size_t)n * sizeof *a->fn_minsp);
    a->fn_frame = calloc((size_t)n, sizeof *a->fn_frame);
    a->fn_incl = calloc((size_t)n, sizeof *a->fn_incl);
    if (!a->fn_minsp || !a->fn_frame || !a->fn_incl)
        die("out of memory");
    for (int i = 0; i < n; i++)
        a->fn_minsp[i] = 0xffffffffu;
    u32 msp, psp;
    an_sps(a, &msp, &psp);
    a->msp_top = msp;               /* the reset value: 0 but on a Cortex-M */
    a->tail_fn = -1;
    stk_entered(a, a->s->cpu->ops->pc(a->s->cpu), 0);
    a->msp_min = msp ? msp : 0xffffffffu;
    a->psp_min = 0xffffffffu;
    if (a->stk_limit_arg) {
        /* --stack-limit: an address, or a symbol's */
        char *e;
        unsigned long v = strtoul(a->stk_limit_arg, &e, 0);
        if (!*e && e != a->stk_limit_arg)
            a->stk_limit = (u32)v;
        else if (!an_data_sym(a, a->stk_limit_arg, &a->stk_limit))
            die("--stack-limit: '%s' is neither an address nor a symbol of %s",
                a->stk_limit_arg, a->s->image);
        a->stk_limit_why = "--stack-limit";
    }
    if (!a->stk_limit_why) {
        for (int i = 0; limit_syms[i] && !a->stk_limit_why; i++)
            if (an_data_sym(a, limit_syms[i], &a->stk_limit))
                a->stk_limit_why = limit_syms[i];
    }
}

/* the end of .data and .bss, when the image says where it is and it
 * lies below the stack's top: 0, or 1 with its address and name */
static int data_end(struct analysis *a, u32 top, u32 *at, const char **why)
{
    for (int i = 0; end_syms[i]; i++) {
        u32 v;
        if (an_data_sym(a, end_syms[i], &v) && v < top &&
            top - v < 0x10000000u) {
            *at = v;
            *why = end_syms[i];
            return 1;
        }
    }
    return 0;
}

/* the limit an overflow is checked against, once the top is known: the
 * given or linked limit, and the end of the data when that is higher */
static void set_check(struct analysis *a)
{
    u32 de;
    const char *why;
    a->stk_check = a->stk_limit;
    a->stk_check_why = a->stk_limit_why;
    if (a->msp_top && data_end(a, a->msp_top, &de, &why) &&
        (!a->stk_limit_why || de > a->stk_limit)) {
        a->stk_check = de;
        a->stk_check_why = why;
        a->stk_check_data = 1;
    }
    a->stk_checked_top = a->msp_top;
}

/* the pc reached pc1 by a jump (not a call): when that is a function's
 * first instruction, its own frame is measured from here */
void stk_entered(struct analysis *a, u32 pc1, int called)
{
    int f = an_fn(a, pc1);
    if (!called && f >= 0 && a->img->fn[f].addr == pc1) {
        a->tail_fn = f;
        a->tail_sp = an_sp(a);
    }
}

void stk_step(struct analysis *a, int ran, int entered, int sample_frame)
{
    struct sim *s = a->s;
    u32 msp, psp;
    if (an_sps(a, &msp, &psp))
        a->psp_used = 1;
    if (msp) {
        if (msp > a->msp_top)
            a->msp_top = msp;
        if (msp < a->msp_min)
            a->msp_min = msp;
    }
    if (a->psp_used) {
        if (psp > a->psp_top)
            a->psp_top = psp;
        if (psp && psp < a->psp_min)
            a->psp_min = psp;
    }
    u32 sp = an_sp(a);
    if (ran && sp) {
        int f = an_fn(a, a->pc0);
        int k = f >= 0 ? f : a->img->nfn;
        if (sp < a->fn_minsp[k])
            a->fn_minsp[k] = sp;
        if (!entered && a->nfr) {
            struct an_frame *t = &a->fr[a->nfr - 1];
            if (sp < t->min)
                t->min = sp;
        }
        /* the function's own frame: below the sp it was entered with, by
         * a call (its frame's) or by a jump to its first instruction */
        u32 base = 0;
        if (a->nfr && a->node[a->fr[a->nfr - 1].node].fn == f && f >= 0)
            base = a->fr[a->nfr - 1].sp + RET_BYTES(a);
        else if (f >= 0 && f == a->tail_fn)
            base = a->tail_sp;
        if (!entered && sample_frame && base && base - sp < 0x80000000u &&
            base - sp > a->fn_frame[k])
            a->fn_frame[k] = base - sp;
    }
    if (!entered)
        stk_entered(a, s->cpu->ops->pc(s->cpu), !sample_frame);
    /* the overflow: the main stack below its limit */
    if (a->msp_top != a->stk_checked_top)
        set_check(a);
    if (msp && a->stk_check && msp < a->stk_check && !a->ovf && s->state == RUN) {
        char where[256];
        a->ovf = 1;
        a->ovf_pc = a->pc0;
        a->ovf_sp = msp;
        a->ovf_entry = !ran;
        image_where(a->img, a->pc0, where, sizeof where);
        sim_end(s, END_LOCKUP, "stack overflow: sp 0x%08x is below %s 0x%08x "
                "(%s), %s %s", msp,
                a->stk_check_data ? "the end of .data and .bss" : "the stack limit",
                a->stk_check, a->stk_check_why,
                ran ? "at" : "in an exception's entry, at", where);
    }
}

/* a frame is over: its depth below its entry, and its lowest sp to the
 * frame that called it (not to the one an exception interrupted) */
void stk_pop(struct analysis *a, int i)
{
    struct an_frame *f = &a->fr[i];
    int fn = a->node[f->node].fn, k = fn >= 0 ? fn : a->img->nfn;
    u32 base = f->sp + RET_BYTES(a);
    if (base - f->min < 0x80000000u && base - f->min > a->fn_incl[k])
        a->fn_incl[k] = base - f->min;
    if (i > 0 && !f->exc && f->min < a->fr[i - 1].min)
        a->fr[i - 1].min = f->min;
}

/* ---- the static numbers ---------------------------------------------- */

/* a .su line: "file:line[:col]:function<TAB>bytes<TAB>qualifiers" */
static void read_su(struct analysis *a, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f)
        die("cannot read %s", path);
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        char *name = strrchr(line, ':');
        name = name ? name + 1 : line;
        long bytes = strtol(tab + 1, 0, 10);
        int dyn = strstr(tab + 1, "dynamic") != 0;
        a->su = realloc(a->su, (size_t)(a->nsu + 1) * sizeof *a->su);
        if (!a->su)
            die("out of memory");
        a->su[a->nsu].name = malloc(strlen(name) + 1);
        if (!a->su[a->nsu].name)
            die("out of memory");
        strcpy(a->su[a->nsu].name, name);
        a->su[a->nsu].bytes = bytes;
        a->su[a->nsu].dynamic = dyn;
        a->nsu++;
    }
    fclose(f);
}

/* embrt --json: an entry a line, {"name": "N", "kind": "K", "bytes": B ...} */
static void read_embrt(struct analysis *a, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4096];
    if (!f)
        die("cannot read %s", path);
    while (fgets(line, sizeof line, f)) {
        char *n = strstr(line, "\"name\": \""), *b = strstr(line, "\"bytes\": ");
        if (!n || !b || !strstr(line, "\"kind\""))
            continue;
        n += 9;
        char *e = strchr(n, '"');
        if (!e)
            continue;
        *e = 0;
        a->rt = realloc(a->rt, (size_t)(a->nrt + 1) * sizeof *a->rt);
        if (!a->rt)
            die("out of memory");
        a->rt[a->nrt].name = malloc(strlen(n) + 1);
        if (!a->rt[a->nrt].name)
            die("out of memory");
        strcpy(a->rt[a->nrt].name, n);
        a->rt[a->nrt].bytes = strtol(b + 9, 0, 10);
        a->nrt++;
    }
    fclose(f);
}

void stk_read_static(struct analysis *a)
{
    for (int i = 0; i < a->nsu_path; i++)
        read_su(a, a->su_path[i]);
    if (a->embrt_path)
        read_embrt(a, a->embrt_path);
}

static const struct su *su_of(const struct analysis *a, const char *name)
{
    for (int i = 0; i < a->nsu; i++)
        if (!strcmp(a->su[i].name, name))
            return &a->su[i];
    return 0;
}

static long rt_of(const struct analysis *a, const char *name)
{
    for (int i = 0; i < a->nrt; i++)
        if (!strcmp(a->rt[i].name, name))
            return a->rt[i].bytes;
    return -2;
}

/* ---- the report --------------------------------------------------------- */

static const struct analysis *sort_a;

static int by_depth(const void *x, const void *y)
{
    int i = *(const int *)x, j = *(const int *)y;
    u32 a = sort_a->fn_minsp[i], b = sort_a->fn_minsp[j];
    if (a != b)
        return a < b ? -1 : 1;
    return i - j;
}

void stk_report(struct analysis *a)
{
    const struct image *m = a->img;
    int nf = m->nfn + 1;
    /* the frames still open: their depths, innermost first */
    for (int i = a->nfr - 1; i >= 0; i--)
        stk_pop(a, i);
    if (!a->stk_path && !a->stk_report)
        return;
    FILE *o = a->stk_path ? fopen(a->stk_path, "w") : stderr;
    if (!o)
        die("cannot write %s", a->stk_path);
    u32 top = a->msp_top, lim = a->stk_limit;
    const char *why = a->stk_limit_why, *dwhy;
    u32 de;
    if (!why && top && data_end(a, top, &de, &dwhy)) {
        lim = de;
        why = dwhy;
    }
    if (!why && top) {
        struct region *r = bus_region(&a->s->bus, top - 1, 1);
        if (r) {
            lim = r->base;
            why = "the bottom of RAM";
        }
    }
    fprintf(o, "embsim stack: %s\n", a->s->image);
    const char *msp_name = a->arch == AN_ARM ? "main stack (MSP)" : "stack";
    if (a->msp_min == 0xffffffffu || !top) {
        fprintf(o, "%s: not used\n", msp_name);
    } else {
        fprintf(o, "%s: top 0x%08x, deepest 0x%08x: %u bytes used", msp_name,
                top, a->msp_min, top - a->msp_min);
        if (why && lim < top)
            fprintf(o, " of %u (down to 0x%08x, %s)", top - lim, lim, why);
        fprintf(o, "\n");
    }
    if (a->ovf) {
        char where[256];
        image_where(m, a->ovf_pc, where, sizeof where);
        fprintf(o, "OVERFLOW: sp 0x%08x went below 0x%08x (%s), %s %s\n",
                a->ovf_sp, a->stk_check,
                a->stk_check_data ? "the end of .data and .bss" : a->stk_check_why,
                a->ovf_entry ? "in an exception's entry, at" : "at", where);
    }
    if (a->arch == AN_ARM) {
        if (a->psp_used && a->psp_min != 0xffffffffu)
            fprintf(o, "process stack (PSP): top 0x%08x, deepest 0x%08x: %u bytes used\n",
                    a->psp_top, a->psp_min, a->psp_top - a->psp_min);
        else
            fprintf(o, "process stack (PSP): not used\n");
    }
    int *idx = malloc((size_t)nf * sizeof *idx), n = 0;
    if (!idx)
        die("out of memory");
    for (int i = 0; i < nf; i++)
        if (a->fn_minsp[i] != 0xffffffffu)
            idx[n++] = i;
    sort_a = a;
    qsort(idx, (size_t)n, sizeof *idx, by_depth);
    fprintf(o, "by function, deepest first (bytes; frame and incl from the "
               "function's entry):\n");
    fprintf(o, "  %-24s %10s %7s %7s %7s %7s %7s\n", "function", "deepest", "depth",
            "frame", ".su", "incl", "embrt");
    int over = 0;
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        const char *name = i < m->nfn ? m->fn[i].name : "[unknown]";
        char depth[16], su[24], rt[24];
        if (top && a->fn_minsp[i] <= top)
            snprintf(depth, sizeof depth, "%u", top - a->fn_minsp[i]);
        else
            snprintf(depth, sizeof depth, "-");
        const struct su *u = i < m->nfn ? su_of(a, name) : 0;
        long r = i < m->nfn ? rt_of(a, name) : -2;
        int bad = 0;
        if (u) {
            snprintf(su, sizeof su, "%ld%s", u->bytes, u->dynamic ? "+" : "");
            if (!u->dynamic && (long)a->fn_frame[i] > u->bytes)
                bad = 1;
        } else
            snprintf(su, sizeof su, "-");
        if (r >= 0) {
            snprintf(rt, sizeof rt, "%ld", r);
            if ((long)a->fn_incl[i] > r)
                bad = 1;
        } else
            snprintf(rt, sizeof rt, r == -1 ? "none" : "-");
        over += bad;
        fprintf(o, "  %-24s 0x%08x %7s %7u %7s %7u %7s%s\n", name,
                a->fn_minsp[i], depth, a->fn_frame[i], su, a->fn_incl[i], rt,
                bad ? "  ABOVE THE STATIC BOUND" : "");
    }
    if (a->nsu || a->nrt)
        fprintf(o, "%s\n", over ? "the run went deeper than the static numbers allow"
                                : "the run stayed within the static numbers");
    free(idx);
    if (o != stderr)
        fclose(o);
}
