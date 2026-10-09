/* profile.c -- --profile[=FILE]: where the run's instructions and cycles
 * went, by function.
 *
 * an_step gives each step's instructions and cycles -- the change in
 * s->insns and s->cycles, so whatever a step costs beyond its
 * instruction (a taken branch, a WFI's sleep, an AVR interrupt's entry)
 * is in it -- to the call path the pc is on (struct an_node). Every step
 * goes to exactly one path, so the counts add up to --stats' totals,
 * to the instruction and the cycle. From the paths:
 *   - a function's self counts are those of the paths that end in it;
 *   - its inclusive counts, those of every path it is on (once, for a
 *     recursive one), so its callees' are in them;
 *   - its calls, the times a call or an exception's entry reached it.
 * The report is the functions by self cycles; --profile-format=collapsed
 * writes the paths as collapsed stacks ("main;f;g 1234", by cycles, or
 * by instructions with collapsed-insns), which flamegraph.pl, speedscope
 * and inferno read. */
#include <stdlib.h>
#include <string.h>

#include "analysis.h"

struct pfn {
    int fn;
    u64 self_i, self_c, incl_i, incl_c, calls;
};

static const char *fn_name(const struct analysis *a, int fn)
{
    return fn >= 0 ? a->img->fn[fn].name : "[unknown]";
}

static int by_self(const void *x, const void *y)
{
    const struct pfn *a = x, *b = y;
    if (a->self_c != b->self_c)
        return a->self_c > b->self_c ? -1 : 1;
    if (a->incl_c != b->incl_c)
        return a->incl_c > b->incl_c ? -1 : 1;
    return a->fn - b->fn;
}

/* the collapsed stacks: a path's names, root first, into buf */
static char **stacks;

static int by_str(const void *x, const void *y)
{
    return strcmp(stacks[*(const int *)x], stacks[*(const int *)y]);
}

static void path(const struct analysis *a, int n, char *buf, size_t cap)
{
    int chain[4096], k = 0;
    for (; n > 0 && k < 4096; n = a->node[n].parent)
        chain[k++] = n;
    size_t len = 0;
    buf[0] = 0;
    while (k-- > 0) {
        const char *nm = fn_name(a, a->node[chain[k]].fn);
        size_t l = strlen(nm);
        if (len + l + 2 >= cap)
            break;
        if (len)
            buf[len++] = ';';
        memcpy(buf + len, nm, l);
        len += l;
        buf[len] = 0;
    }
}

static void pct(char *b, size_t n, u64 part, u64 all)
{
    snprintf(b, n, "%5.1f%%", all ? 100.0 * (double)part / (double)all : 0.0);
}

void prof_report(struct analysis *a)
{
    struct sim *s = a->s;
    int nf = a->img->nfn + 1;           /* the last: no symbol */
    struct pfn *p = calloc((size_t)nf, sizeof *p);
    int *mark = calloc((size_t)nf, sizeof *mark);
    if (!p || !mark)
        die("out of memory");
    for (int i = 0; i < nf; i++)
        p[i].fn = i < nf - 1 ? i : -1;
    for (int n = 1; n < a->nnode; n++) {
        const struct an_node *d = &a->node[n];
        int f = d->fn >= 0 ? d->fn : nf - 1;
        p[f].self_i += d->insns;
        p[f].self_c += d->cycles;
        p[f].calls += d->calls;
        if (!d->insns && !d->cycles)
            continue;
        /* each function on the path, once */
        for (int up = n; up > 0; up = a->node[up].parent) {
            int g = a->node[up].fn >= 0 ? a->node[up].fn : nf - 1;
            if (mark[g] == n)
                continue;
            mark[g] = n;
            p[g].incl_i += d->insns;
            p[g].incl_c += d->cycles;
        }
    }
    FILE *o = a->prof_path ? fopen(a->prof_path, "w") : stderr;
    if (!o)
        die("cannot write %s", a->prof_path);
    if (a->prof_fmt) {
        /* collapsed stacks, sorted, by cycles (1) or instructions (2) */
        int *idx = calloc((size_t)a->nnode, sizeof *idx), ni = 0;
        stacks = calloc((size_t)a->nnode, sizeof *stacks);
        if (!idx || !stacks)
            die("out of memory");
        for (int n = 1; n < a->nnode; n++) {
            u64 w = a->prof_fmt == 1 ? a->node[n].cycles : a->node[n].insns;
            if (!w)
                continue;
            char buf[16384];
            path(a, n, buf, sizeof buf);
            stacks[n] = malloc(strlen(buf) + 1);
            if (!stacks[n])
                die("out of memory");
            strcpy(stacks[n], buf);
            idx[ni++] = n;
        }
        qsort(idx, (size_t)ni, sizeof *idx, by_str);
        for (int i = 0; i < ni; i++) {
            int n = idx[i];
            fprintf(o, "%s %llu\n", stacks[n],
                    (unsigned long long)(a->prof_fmt == 1 ? a->node[n].cycles
                                                          : a->node[n].insns));
            free(stacks[n]);
        }
        free(idx);
        free(stacks);
    } else {
        qsort(p, (size_t)nf, sizeof *p, by_self);
        char p1[16], p2[16];
        fprintf(o, "embsim profile: %s, %llu instructions, %llu cycles (est.)\n",
                s->image, (unsigned long long)s->insns,
                (unsigned long long)s->cycles);
        fprintf(o, "%12s %12s %6s %12s %12s %6s %9s  %s\n", "self insns",
                "self cycles", "self", "incl insns", "incl cycles", "incl",
                "calls", "function");
        u64 ti = 0, tc = 0;
        for (int i = 0; i < nf; i++) {
            if (!p[i].incl_i && !p[i].incl_c && !p[i].calls)
                continue;
            ti += p[i].self_i;
            tc += p[i].self_c;
            pct(p1, sizeof p1, p[i].self_c, s->cycles);
            pct(p2, sizeof p2, p[i].incl_c, s->cycles);
            fprintf(o, "%12llu %12llu %6s %12llu %12llu %6s %9llu  %s\n",
                    (unsigned long long)p[i].self_i,
                    (unsigned long long)p[i].self_c, p1,
                    (unsigned long long)p[i].incl_i,
                    (unsigned long long)p[i].incl_c, p2,
                    (unsigned long long)p[i].calls, fn_name(a, p[i].fn));
        }
        pct(p1, sizeof p1, tc, s->cycles);
        fprintf(o, "%12llu %12llu %6s %12s %12s %6s %9s  (total)\n",
                (unsigned long long)ti, (unsigned long long)tc, p1, "", "", "",
                "");
    }
    if (o != stderr)
        fclose(o);
    free(p);
    free(mark);
}
