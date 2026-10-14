/* EmbCC's options, read from options.def.
 *
 * The driver's argument loop (main.c) was written against GCC's
 * spellings, and still reads them. EmbCC's own spellings are rewritten
 * into those before it runs (opts_normalize), so the loop, and the code
 * it produces, are the same whichever spelling a build uses. The help and
 * the option reference are printed from the same table, so what the
 * driver accepts and what the documentation says cannot drift apart:
 * tests/golden/options-table.sh holds the driver to the table. */
#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

enum opt_group {
    G_BUILD, G_LANG, G_TARGET, G_OPT, G_DIAG, G_DEBUG, G_DEPS, G_LINK,
    G_INFO, G_COUNT
};

static const char *const group_title[G_COUNT] = {
    "What to build", "The language", "The target", "Optimization",
    "Warnings and diagnostics", "Debugging and reports", "Dependencies",
    "Linking", "Asking EmbCC"
};

struct opt {
    enum opt_group group;
    const char *spelling;   /* EmbCC's; NULL in a GCC-only row */
    const char *arg;        /* the value's name, or NULL */
    const char *gcc;        /* the other spelling, as a rewrite template */
    const char *help;
};

static const struct opt opts[] = {
#define OPT(g, s, a, c, h) { G_##g, s, a, c, h },
#define GCCONLY(g, s, h)   { G_##g, NULL, NULL, s, h },
#include "options.def"
#undef OPT
#undef GCCONLY
};
#define NOPTS ((int)(sizeof opts / sizeof opts[0]))

/* The rewritten argument vector. */
struct strvec { char **v; int n, cap; };
static void strvec_push(struct strvec *s, char *a)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? 2 * s->cap : 32;
        s->v = xrealloc(s->v, (size_t)s->cap * sizeof *s->v);
    }
    s->v[s->n++] = a;
}

static int takes_value(const struct opt *o)
{
    size_t n = strlen(o->spelling);
    return n > 0 && o->spelling[n - 1] == '=';
}

/* Append the arguments GCC's template t stands for, with '%' replaced by
 * the value v: "-MMD -MF %" and "foo.d" are three arguments. */
static void expand(struct strvec *out, const char *t, const char *v)
{
    while (*t) {
        const char *e = strchr(t, ' ');
        size_t n = e ? (size_t)(e - t) : strlen(t);
        const char *pct = memchr(t, '%', n);
        size_t vn = pct ? strlen(v) : 0;
        char *a = xmalloc(n + vn + 1);
        if (pct) {
            size_t pre = (size_t)(pct - t);
            memcpy(a, t, pre);
            memcpy(a + pre, v, vn);
            memcpy(a + pre + vn, pct + 1, n - pre - 1);
            a[n - 1 + vn] = 0;
        } else {
            memcpy(a, t, n);
            a[n] = 0;
        }
        strvec_push(out, a);
        t += n;
        while (*t == ' ')
            t++;
    }
}

/* The row whose EmbCC spelling arg is, exactly; then one whose spelling
 * ends in '=' and begins arg (*val: the rest); then one whose spelling is
 * arg with an '=' after it, the value in the next argument (*sep = 1). */
static const struct opt *lookup(const char *arg, const char **val, int *sep)
{
    *val = NULL;
    *sep = 0;
    for (int k = 0; k < NOPTS; k++)
        if (opts[k].spelling && !takes_value(&opts[k]) &&
            strcmp(arg, opts[k].spelling) == 0)
            return &opts[k];
    for (int k = 0; k < NOPTS; k++) {
        if (!opts[k].spelling || !takes_value(&opts[k]))
            continue;
        size_t n = strlen(opts[k].spelling);
        if (strncmp(arg, opts[k].spelling, n) == 0) {
            *val = arg + n;
            return &opts[k];
        }
        if (strncmp(arg, opts[k].spelling, n - 1) == 0 && arg[n - 1] == 0) {
            *sep = 1;
            return &opts[k];
        }
    }
    return NULL;
}

int opts_normalize(int *argc, char ***argv)
{
    struct strvec out = { 0, 0, 0 };
    int changed = 0;
    for (int i = 0; i < *argc; i++) {
        const char *a = (*argv)[i];
        const char *val;
        int sep;
        const struct opt *o = i > 0 && a[0] == '-' ? lookup(a, &val, &sep)
                                                   : NULL;
        if (!o || !o->gcc) {
            strvec_push(&out, (*argv)[i]);   /* the loop reads it itself */
            continue;
        }
        if (sep) {
            if (i + 1 >= *argc) {
                fprintf(stderr, "embcc: error: %s needs %s\n", a,
                        o->arg ? o->arg : "a value");
                return -1;
            }
            val = (*argv)[++i];
        }
        expand(&out, o->gcc, val ? val : "");
        changed = 1;
    }
    if (!changed) {
        free(out.v);
        return 0;
    }
    strvec_push(&out, NULL);
    *argc = out.n - 1;
    *argv = out.v;
    return 0;
}

/* "--cpu=CPU", "-o FILE", "-c": a spelling as the help shows it. */
static void spell(char *buf, size_t n, const struct opt *o)
{
    if (!o->arg)
        snprintf(buf, n, "%s", o->spelling);
    else if (takes_value(o))
        snprintf(buf, n, "%s%s", o->spelling, o->arg);
    else
        snprintf(buf, n, "%s %s", o->spelling, o->arg);
}

void opts_print_help(FILE *out)
{
    char b[96];
    for (int g = 0; g < G_COUNT; g++) {
        fprintf(out, "\n%s\n", group_title[g]);
        for (int k = 0; k < NOPTS; k++) {
            if (opts[k].group != (enum opt_group)g || !opts[k].spelling)
                continue;
            spell(b, sizeof b, &opts[k]);
            if (strlen(b) <= 26)
                fprintf(out, "  %-26s %s\n", b, opts[k].help);
            else
                fprintf(out, "  %s\n  %-26s %s\n", b, "", opts[k].help);
        }
    }
    fprintf(out, "\nGCC's spellings of these work too: --help=gcc lists "
                 "them, and the ones EmbCC has no name for.\n");
}

/* A rewrite template as a person would write it: '%' is the value. */
static void shown(char *buf, size_t n, const char *t, const char *arg)
{
    size_t k = 0;
    for (; *t && k + 1 < n; t++) {
        const char *s = *t == '%' ? (arg ? arg : "VALUE") : NULL;
        if (!s) {
            buf[k++] = *t;
            continue;
        }
        while (*s && k + 1 < n)
            buf[k++] = *s++;
    }
    buf[k] = 0;
}

void opts_print_gcc(FILE *out)
{
    fprintf(out, "Other spellings EmbCC accepts (GCC's, and its own older "
                 "GCC-style ones), and EmbCC's for the same thing:\n\n");
    for (int k = 0; k < NOPTS; k++) {
        if (!opts[k].spelling || !opts[k].gcc)
            continue;
        char b[96], g[96];
        spell(b, sizeof b, &opts[k]);
        shown(g, sizeof g, opts[k].gcc, opts[k].arg);
        fprintf(out, "  %-34s %s\n", g, b);
    }
    fprintf(out, "\nGCC's spellings EmbCC reads that have no EmbCC name:\n\n");
    for (int k = 0; k < NOPTS; k++)
        if (!opts[k].spelling)
            fprintf(out, "  %-34s %s\n", opts[k].gcc, opts[k].help);
    fprintf(out, "\nEach target's -m options are GCC's too: docs/manual/"
                 "targets.md lists them.\n");
}

/* A Markdown table cell: '|' escaped, '%' shown as the value's name. */
static void cell(FILE *out, const char *s, const char *arg)
{
    fputc('`', out);
    for (; *s; s++) {
        if (*s == '|')
            fputs("\\|", out);
        else if (*s == '%')
            fputs(arg ? arg : "VALUE", out);
        else
            fputc(*s, out);
    }
    fputc('`', out);
}

void opts_print_markdown(FILE *out)
{
    fprintf(out,
        "# Option reference\n\n"
        "Every option `embcc` takes, generated from the driver's own table "
        "(`src/driver/options.def`) by `embcc --help=markdown`, so it "
        "lists exactly what the driver accepts.\n\n"
        "EmbCC's own spellings come first. Where GCC spells the same "
        "thing differently, its spelling is accepted too and is shown "
        "beside it (with EmbCC's own older GCC-style ones), so a build "
        "written for GCC keeps working; "
        "[Migrating from GCC](migrating-from-gcc.md) covers the rest.\n");
    for (int g = 0; g < G_COUNT; g++) {
        fprintf(out, "\n## %s\n\n| Option | What it does | Also accepted |\n"
                     "|---|---|---|\n", group_title[g]);
        for (int k = 0; k < NOPTS; k++) {
            if (opts[k].group != (enum opt_group)g || !opts[k].spelling)
                continue;
            char b[96];
            spell(b, sizeof b, &opts[k]);
            fputs("| ", out);
            cell(out, b, NULL);
            fprintf(out, " | %s | ", opts[k].help);
            if (opts[k].gcc)
                cell(out, opts[k].gcc, opts[k].arg);
            fputs(" |\n", out);
        }
    }
    fprintf(out, "\n## GCC spellings with no EmbCC name\n\n"
                 "GCC flags EmbCC reads, either because they change nothing "
                 "here or so as to refuse them by name.\n\n"
                 "| GCC spelling | Notes |\n|---|---|\n");
    for (int k = 0; k < NOPTS; k++)
        if (!opts[k].spelling) {
            fputs("| ", out);
            cell(out, opts[k].gcc, NULL);
            fprintf(out, " | %s |\n", opts[k].help);
        }
}

/* For the drift test: every spelling the table knows, one per line --
 * EmbCC's, then each GCC alias's first argument, then the GCC-only ones
 * (a trailing '*' or '%' marks a family). */
void opts_print_spellings(FILE *out)
{
    for (int k = 0; k < NOPTS; k++) {
        if (opts[k].spelling)
            fprintf(out, "ours %s\n", opts[k].spelling);
        if (opts[k].gcc) {
            const char *sp = strchr(opts[k].gcc, ' ');
            int n = sp ? (int)(sp - opts[k].gcc) : (int)strlen(opts[k].gcc);
            fprintf(out, "%s %.*s\n", opts[k].spelling ? "alias" : "gcc",
                    n, opts[k].gcc);
        }
    }
}
