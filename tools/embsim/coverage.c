/* coverage.c -- --coverage FILE: which source lines and functions ran.
 *
 * an_step counts each instruction executed, by its address, in a count
 * per halfword of the image's code (its executable segments). At the end
 * the line table maps the counts to source lines: a line has code when
 * the table gives it an address range in the code, and its count is
 * that of its most-executed instruction -- the times the line ran, for
 * straight-line code. A function's count is its first instruction's:
 * the times it was entered. The report is gcov's layout, per file, with
 * the source beside the counts when the file can be read; or, with
 * --coverage-format=lcov, lcov's tracefile, which genhtml and the CI
 * coverage services read. */
#include <stdlib.h>
#include <string.h>

#include "analysis.h"

void cov_init(struct analysis *a)
{
    struct image *m = a->img;
    if (!m->ncode)
        die("--coverage: %s has no executable segment", a->s->image);
    if (!m->nln)
        fprintf(stderr, "embsim: warning: %s has no line table (build it "
                "with -g): --coverage reports functions only\n", a->s->image);
    for (int i = 0; i < m->ncode; i++) {
        a->cov[i] = calloc((size_t)(m->code_hi[i] - m->code_lo[i]) / 2 + 1,
                           sizeof *a->cov[i]);
        if (!a->cov[i])
            die("out of memory");
    }
}

/* the count of the instruction at `pc` (0 outside the code) */
static u64 count_at(const struct analysis *a, u32 pc)
{
    const struct image *m = a->img;
    for (int i = 0; i < m->ncode; i++)
        if (pc - m->code_lo[i] < m->code_hi[i] - m->code_lo[i])
            return a->cov[i][(pc - m->code_lo[i]) >> 1];
    return 0;
}

static int code_range(const struct image *m, u32 lo, u32 hi)
{
    for (int i = 0; i < m->ncode; i++)
        if (lo >= m->code_lo[i] && hi <= m->code_hi[i])
            return 1;
    return 0;
}

/* a file's lines: whether each has code, and its count */
struct fline {
    int code;
    u64 count;
};

struct cfile {
    int idx;                        /* in m->file */
    struct fline *ln;
    int nln;
    int *fn;                        /* its functions, by index in m->fn */
    int nfn;
};

/* the files by path: qsort's comparison sees the names through this */
static char **cov_files;

static int by_path(const void *x, const void *y)
{
    const struct cfile *a = x, *b = y;
    return strcmp(cov_files[a->idx], cov_files[b->idx]);
}

static void pct(char *buf, size_t n, int hit, int all)
{
    if (all)
        snprintf(buf, n, "%.1f%%", 100.0 * hit / all);
    else
        snprintf(buf, n, "-");
}

void cov_report(struct analysis *a)
{
    struct image *m = a->img;
    struct cfile *cf = calloc((size_t)m->nfile + 1, sizeof *cf);
    if (!cf)
        die("out of memory");
    for (int i = 0; i < m->nfile; i++)
        cf[i].idx = i;
    /* the lines */
    for (int i = 0; i < m->nln; i++) {
        const struct img_line *l = &m->ln[i];
        if (l->line <= 0 || !code_range(m, l->lo, l->hi))
            continue;
        struct cfile *f = &cf[l->file];
        if (l->line >= f->nln) {
            int n = l->line + 64;
            f->ln = realloc(f->ln, (size_t)n * sizeof *f->ln);
            if (!f->ln)
                die("out of memory");
            memset(f->ln + f->nln, 0, (size_t)(n - f->nln) * sizeof *f->ln);
            f->nln = n;
        }
        struct fline *fl = &f->ln[l->line];
        fl->code = 1;
        for (u32 pc = l->lo; pc < l->hi; pc += 2) {
            u64 c = count_at(a, pc);
            if (c > fl->count)
                fl->count = c;
        }
    }
    /* the functions, in the file of their first line; the ones without
     * a line go in their own list */
    int *nofile = calloc((size_t)m->nfn + 1, sizeof *nofile), nnofile = 0;
    if (!nofile)
        die("out of memory");
    int fn_all = 0, fn_hit = 0;
    for (int i = 0; i < m->nfn; i++) {
        if (!code_range(m, m->fn[i].addr, m->fn[i].addr + 1))
            continue;
        const struct img_line *l = image_line(m, m->fn[i].addr);
        fn_all++;
        fn_hit += count_at(a, m->fn[i].addr) != 0;
        if (!l) {
            nofile[nnofile++] = i;
            continue;
        }
        struct cfile *f = &cf[l->file];
        f->fn = realloc(f->fn, (size_t)(f->nfn + 1) * sizeof *f->fn);
        if (!f->fn)
            die("out of memory");
        f->fn[f->nfn++] = i;
    }
    cov_files = m->file;
    qsort(cf, (size_t)m->nfile, sizeof *cf, by_path);

    FILE *o = strcmp(a->cov_path, "-") ? fopen(a->cov_path, "w") : stderr;
    if (!o)
        die("cannot write %s", a->cov_path);
    int ln_all = 0, ln_hit = 0;
    char p1[16], p2[16];
    if (!a->cov_lcov)
        fprintf(o, "embsim coverage: %s, %llu instructions run\n",
                a->s->image, (unsigned long long)a->s->insns);
    else
        fprintf(o, "TN:\n");
    for (int i = 0; i < m->nfile; i++) {
        struct cfile *f = &cf[i];
        int all = 0, hit = 0, fh = 0;
        for (int k = 0; k < f->nln; k++)
            if (f->ln[k].code) {
                all++;
                hit += f->ln[k].count != 0;
            }
        if (!all && !f->nfn)
            continue;
        for (int k = 0; k < f->nfn; k++)
            fh += count_at(a, m->fn[f->fn[k]].addr) != 0;
        ln_all += all;
        ln_hit += hit;
        const char *path = m->file[f->idx];
        if (a->cov_lcov) {
            fprintf(o, "SF:%s\n", path);
            for (int k = 0; k < f->nfn; k++) {
                const struct img_sym *s = &m->fn[f->fn[k]];
                fprintf(o, "FN:%d,%s\n", image_line(m, s->addr)->line, s->name);
            }
            for (int k = 0; k < f->nfn; k++) {
                const struct img_sym *s = &m->fn[f->fn[k]];
                fprintf(o, "FNDA:%llu,%s\n",
                        (unsigned long long)count_at(a, s->addr), s->name);
            }
            fprintf(o, "FNF:%d\nFNH:%d\n", f->nfn, fh);
            for (int k = 0; k < f->nln; k++)
                if (f->ln[k].code)
                    fprintf(o, "DA:%d,%llu\n", k,
                            (unsigned long long)f->ln[k].count);
            fprintf(o, "LF:%d\nLH:%d\nend_of_record\n", all, hit);
            continue;
        }
        pct(p1, sizeof p1, hit, all);
        pct(p2, sizeof p2, fh, f->nfn);
        fprintf(o, "\n%s: %d of %d lines (%s), %d of %d functions (%s)\n",
                path, hit, all, p1, fh, f->nfn, p2);
        for (int k = 0; k < f->nfn; k++) {
            const struct img_sym *s = &m->fn[f->fn[k]];
            fprintf(o, "  function %s (line %d): %llu\n", s->name,
                    image_line(m, s->addr)->line,
                    (unsigned long long)count_at(a, s->addr));
        }
        /* the source beside the counts, as gcov lays it out */
        FILE *src = fopen(path, "r");
        int no = 0, c = 0;
        if (src) {
            while (c != EOF) {
                char line[512];
                int len = 0;
                while ((c = fgetc(src)) != EOF && c != '\n')
                    if (len < (int)sizeof line - 1)
                        line[len++] = (char)c;
                line[len] = 0;
                if (c == EOF && !len)
                    break;
                no++;
                if (no < f->nln && f->ln[no].code && f->ln[no].count)
                    fprintf(o, "%9llu:%5d:%s\n",
                            (unsigned long long)f->ln[no].count, no, line);
                else if (no < f->nln && f->ln[no].code)
                    fprintf(o, "%9s:%5d:%s\n", "#####", no, line);
                else
                    fprintf(o, "%9s:%5d:%s\n", "-", no, line);
            }
            fclose(src);
        }
        /* the lines the source did not show (no source, or past its end) */
        for (int k = no + 1; k < f->nln; k++) {
            if (!f->ln[k].code)
                continue;
            if (f->ln[k].count)
                fprintf(o, "%9llu:%5d:\n", (unsigned long long)f->ln[k].count, k);
            else
                fprintf(o, "%9s:%5d:\n", "#####", k);
        }
    }
    if (!a->cov_lcov) {
        if (nnofile) {
            fprintf(o, "\nfunctions with no line information:\n");
            for (int k = 0; k < nnofile; k++)
                fprintf(o, "  function %s: %llu\n", m->fn[nofile[k]].name,
                        (unsigned long long)count_at(a, m->fn[nofile[k]].addr));
        }
        pct(p1, sizeof p1, ln_hit, ln_all);
        pct(p2, sizeof p2, fn_hit, fn_all);
        fprintf(o, "\ntotal: %d of %d lines (%s), %d of %d functions (%s)\n",
                ln_hit, ln_all, p1, fn_hit, fn_all, p2);
    }
    if (o != stderr)
        fclose(o);
    for (int i = 0; i < m->nfile; i++) {
        free(cf[i].ln);
        free(cf[i].fn);
    }
    free(cf);
    free(nofile);
}
