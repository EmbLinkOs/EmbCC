/* The diagnostic engine (docs/tools/diagnostics.md T1).
 *
 * A diagnostic is a record, not a line printed on the way out: severity, a
 * location with a source range, the option that controls it, notes under
 * it, and fix-its — a replacement for a span of the source, which an editor
 * can apply without parsing English. Records are held and rendered once, so
 * the same compile can print GCC's caret output or GCC's JSON, and so a
 * fix-it attached after its diagnostic was built still reaches the reader.
 *
 * The ~300 places that call an error and then exit(1) need no change: the
 * flush runs from atexit. What they print is what they always printed.
 */
#include "util.h"
#include "../platform/platform.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* isatty — auto colour when stderr is a terminal */

/* ---- source registry -------------------------------------------------------
 *
 * Diagnostics render the offending source line, so the text of every file a
 * diagnostic can name is registered here under the exact name the lexer reports
 * (the driver registers the main file; the preprocessor each #include). A
 * diagnostic with an unregistered file (a synthetic location) still prints its
 * message and location — just no source line. */
struct src_ent { const char *file; const char *text; };
static struct src_ent *g_srcs;
static int g_nsrc, g_capsrc;

void diag_register_source(const char *file, const char *text)
{
    for (int i = 0; i < g_nsrc; i++)
        if (!strcmp(g_srcs[i].file, file)) { g_srcs[i].text = text; return; }
    if (g_nsrc == g_capsrc) {
        g_capsrc = g_capsrc ? g_capsrc * 2 : 8;
        g_srcs = xrealloc(g_srcs, (size_t)g_capsrc * sizeof *g_srcs);
    }
    g_srcs[g_nsrc].file = file;
    g_srcs[g_nsrc].text = text;
    g_nsrc++;
}

/* Start of 1-based `line` in `file` (NUL/newline-terminated run), or NULL. */
static const char *src_line(const char *file, int line, int *len_out)
{
    const char *text = NULL;
    for (int i = 0; i < g_nsrc; i++)
        if (!strcmp(g_srcs[i].file, file)) { text = g_srcs[i].text; break; }
    if (!text || line < 1)
        return NULL;
    const char *p = text;
    for (int n = 1; n < line; n++) {
        p = strchr(p, '\n');
        if (!p) return NULL;
        p++;
    }
    const char *e = p;
    while (*e && *e != '\n') e++;
    *len_out = (int)(e - p);
    return p;
}

/* ---- system headers ---------------------------------------------------
 *
 * A warning about code in a header the project did not write is not
 * something its author can act on, so — as GCC does — warnings from system
 * headers are dropped unless -Wsystem-headers asks for them. Errors are
 * never dropped: those stop the build wherever they are. */
static const char **g_sysfiles;
static int g_nsysfiles, g_capsysfiles;
static int g_warn_system;

void diag_mark_system(const char *file)
{
    if (g_nsysfiles == g_capsysfiles) {
        g_capsysfiles = g_capsysfiles ? g_capsysfiles * 2 : 32;
        g_sysfiles = xrealloc(g_sysfiles,
                              (size_t)g_capsysfiles * sizeof *g_sysfiles);
    }
    g_sysfiles[g_nsysfiles++] = file;
}

void diag_set_warn_system(int on) { g_warn_system = on; }

static int in_system_header(const char *file)
{
    if (g_warn_system || !file)
        return 0;
    for (int i = 0; i < g_nsysfiles; i++)
        if (!strcmp(g_sysfiles[i], file))
            return 1;
    return 0;
}

/* ---- macro-expansion registry ----
 *
 * The parser runs on preprocessed text, so an error inside a macro expansion is
 * reported at the INVOCATION line — a line that often looks fine, because the
 * offending code is in the macro body. The preprocessor records each expansion
 * here (file, line -> macro name); an error at that line then adds a bare
 * "expanded from macro 'X'" note, explaining the caret. Line-granularity, so it
 * fires only when the macro's name actually appears on the error's line. */
struct exp_ent { const char *file; int line; const char *macro; };
static struct exp_ent *g_exps;
static int g_nexp, g_capexp;

void diag_register_expansion(const char *file, int line, const char *macro)
{
    if (g_nexp == g_capexp) {
        g_capexp = g_capexp ? g_capexp * 2 : 16;
        g_exps = xrealloc(g_exps, (size_t)g_capexp * sizeof *g_exps);
    }
    g_exps[g_nexp].file = file;
    g_exps[g_nexp].line = line;
    g_exps[g_nexp].macro = macro;
    g_nexp++;
}

/* 1-based column of `needle`'s first whole-identifier occurrence in hay[0..len),
 * or 0 if absent. */
static int word_col(const char *hay, int len, const char *needle)
{
    int nl = (int)strlen(needle);
    for (int i = 0; i + nl <= len; i++) {
        if (memcmp(hay + i, needle, (size_t)nl) != 0)
            continue;
        int lok = i == 0 || !(isalnum((unsigned char)hay[i - 1]) || hay[i - 1] == '_');
        int rok = i + nl == len ||
                  !(isalnum((unsigned char)hay[i + nl]) || hay[i + nl] == '_');
        if (lok && rok)
            return i + 1;
    }
    return 0;
}

/* The macro whose expansion an error at file:line:col came from, or NULL. An
 * error inside an expansion is reported at the macro's start column, so this
 * fires only when the column lands within the macro name on that line — which
 * keeps an unrelated error that merely shares a line with some macro from being
 * misattributed. (col 0 = column unknown: fall back to name-on-line.) */
static const char *expanded_from(const char *file, int line, int col)
{
    for (int i = g_nexp - 1; i >= 0; i--) {
        if (g_exps[i].line != line || strcmp(g_exps[i].file, file) != 0)
            continue;
        int len;
        const char *ln = src_line(file, line, &len);
        if (!ln)
            continue;
        const char *m = g_exps[i].macro;
        int mc = word_col(ln, len, m);
        if (mc == 0)
            continue;
        if (col > 0 && !(col >= mc && col < mc + (int)strlen(m)))
            continue;
        return m;
    }
    return NULL;
}

/* ---- the records ---- */

struct fixit {
    int line, col, end_col;        /* replace [col, end_col) on this line */
    char *text;
};

struct diag {
    int level;                     /* DIAG_* */
    const char *file;
    int line, col, end_col;        /* end_col 0: infer the token's extent */
    char *msg;
    const char *option;            /* the -W that controls it, or NULL */
    const char *id;                /* "E0001": `embcc --explain` knows it */
    struct fixit *fixits;
    int nfixits;
    struct diag *notes;            /* children, in order */
    int nnotes, capnotes;
};

static struct diag *g_diags;
static int g_ndiag, g_capdiag;
static int g_errors, g_warnings;
static int g_format = DIAG_TEXT;
static int g_color = -1;           /* -1 auto, 0 never, 1 always */
static int g_max_errors;           /* 0: no limit */
static int g_werror, g_no_warnings, g_parseable_fixits;
static int g_flushed;

static char *vfmt(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0)
        n = 0;
    char *p = xmalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    return p;
}

/* The diagnostic a note attaches to: the last one recorded. */
static struct diag *last_diag(void)
{
    return g_ndiag ? &g_diags[g_ndiag - 1] : NULL;
}

/* The record a fix-it attaches to: the last note of the last diagnostic, or
 * the diagnostic itself — clang hangs "did you mean 'x'?"'s fix-it on the
 * note, and so does this. */
static struct diag *fixit_target(void)
{
    struct diag *d = last_diag();
    if (!d)
        return NULL;
    return d->nnotes ? &d->notes[d->nnotes - 1] : d;
}

static struct diag *new_diag(int level, const char *file, int line, int col,
                             char *msg)
{
    if (level == DIAG_NOTE && g_ndiag) {
        struct diag *p = last_diag();
        if (p->nnotes == p->capnotes) {
            p->capnotes = p->capnotes ? p->capnotes * 2 : 4;
            p->notes = xrealloc(p->notes, (size_t)p->capnotes * sizeof *p->notes);
        }
        struct diag *n = &p->notes[p->nnotes++];
        memset(n, 0, sizeof *n);
        n->level = level;
        n->file = file;
        n->line = line;
        n->col = col;
        n->msg = msg;
        return n;
    }
    if (g_ndiag == g_capdiag) {
        g_capdiag = g_capdiag ? g_capdiag * 2 : 16;
        g_diags = xrealloc(g_diags, (size_t)g_capdiag * sizeof *g_diags);
    }
    struct diag *d = &g_diags[g_ndiag++];
    memset(d, 0, sizeof *d);
    d->level = level;
    d->file = file;
    d->line = line;
    d->col = col;
    d->msg = msg;
    if (level == DIAG_ERROR)
        g_errors++;
    else if (level == DIAG_WARNING)
        g_warnings++;
    /* the expansion this location came from, as a note under it */
    if (level != DIAG_NOTE && file) {
        const char *m = expanded_from(file, line, col);
        if (m) {
            int save = g_ndiag;
            (void)save;
            char *nm = xmalloc(strlen(m) + 40);
            sprintf(nm, "expanded from macro '%s'", m);
            struct diag *n;
            if (d->nnotes == d->capnotes) {
                d->capnotes = d->capnotes ? d->capnotes * 2 : 4;
                d->notes = xrealloc(d->notes,
                                    (size_t)d->capnotes * sizeof *d->notes);
            }
            n = &d->notes[d->nnotes++];
            memset(n, 0, sizeof *n);
            n->level = DIAG_NOTE;
            n->file = file;
            n->line = line;
            n->col = 0;              /* the heading only, as before */
            n->msg = nm;
            n->end_col = -1;         /* (a bare note: no source line) */
        }
    }
    return d;
}

/* ---- rendering: text ---- */

static const char *cc(const char *code)
{
    if (g_color < 0)
        g_color = isatty(2) ? 1 : 0;
    return g_color ? code : "";
}

static const char *level_name(int level)
{
    return level == DIAG_ERROR ? "error"
         : level == DIAG_WARNING ? "warning" : "note";
}

static const char *level_sgr(int level)
{
    return level == DIAG_ERROR ? "\033[1;31m"
         : level == DIAG_WARNING ? "\033[1;35m" : "\033[1;36m";
}

/* The columns a caret marks: [col, end) — the given range, or the extent of
 * the identifier or number the caret lands on. */
static int caret_end(const struct diag *d, const char *ln, int len)
{
    if (d->end_col > d->col)
        return d->end_col;
    int c = d->col - 1;
    if (c >= 0 && c < len && (isalnum((unsigned char)ln[c]) || ln[c] == '_')) {
        int j = c + 1;
        while (j < len && (isalnum((unsigned char)ln[j]) || ln[j] == '_'))
            j++;
        return j + 1;
    }
    return d->col + 1;
}

static void render_text(const struct diag *d)
{
    fprintf(stderr, "embcc: ");
    fprintf(stderr, "%s%s", cc("\033[1m"), d->file ? d->file : "<embcc>");
    if (d->line > 0) fprintf(stderr, ":%d", d->line);
    if (d->col > 0)  fprintf(stderr, ":%d", d->col);
    fprintf(stderr, ":%s %s%s:%s %s%s", cc("\033[0m"), cc(level_sgr(d->level)),
            level_name(d->level), cc("\033[0m"), cc("\033[1m"), d->msg);
    if (d->option)
        fprintf(stderr, " [%s]", d->option);
    if (d->id)
        fprintf(stderr, " [%s]", d->id);
    fprintf(stderr, "%s\n", cc("\033[0m"));

    int len;
    const char *ln = d->end_col == -1 || d->line <= 0 || !d->file
                     ? NULL : src_line(d->file, d->line, &len);
    if (!ln)
        return;
    fprintf(stderr, "  %.*s\n", len, ln);
    if (d->col > 0 && d->col <= len + 1) {   /* a caret only when known */
        int end = caret_end(d, ln, len);
        fputs("  ", stderr);
        for (int i = 1; i < d->col; i++)
            fputc(ln[i - 1] == '\t' ? '\t' : ' ', stderr);
        fprintf(stderr, "%s^", cc("\033[1;32m"));
        for (int j = d->col + 1; j < end; j++)
            fputc('~', stderr);
        fprintf(stderr, "%s\n", cc("\033[0m"));
    }
    /* A fix-it prints under the caret, as GCC prints it: the replacement
     * text where it goes, so the eye reads the edit. */
    for (int i = 0; i < d->nfixits; i++) {
        const struct fixit *f = &d->fixits[i];
        if (f->line != d->line || f->col <= 0)
            continue;
        fputs("  ", stderr);
        for (int k = 1; k < f->col; k++)
            fputc(k - 1 < len && ln[k - 1] == '\t' ? '\t' : ' ', stderr);
        fprintf(stderr, "%s%s%s\n", cc("\033[1;32m"), f->text, cc("\033[0m"));
    }
}

/* ---- rendering: JSON (GCC's -fdiagnostics-format=json schema) ---- */

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20)
                fprintf(f, "\\u%04x", c);
            else
                fputc((int)c, f);
        }
    }
    fputc('"', f);
}

static void json_point(FILE *f, const char *name, const char *file, int line,
                       int col)
{
    fprintf(f, "\"%s\": {\"file\": ", name);
    json_str(f, file ? file : "<embcc>");
    fprintf(f, ", \"line\": %d, \"column\": %d, \"display-column\": %d, "
               "\"byte-column\": %d}", line, col, col, col);
}

static void render_json(FILE *f, const struct diag *d, int indent)
{
    const char *pad = indent ? "      " : "  ";
    fprintf(f, "%s{\"kind\": ", pad);
    json_str(f, level_name(d->level));
    fprintf(f, ", \"message\": ");
    json_str(f, d->msg);
    if (d->option) {
        fprintf(f, ", \"option\": ");
        json_str(f, d->option);
    }
    if (d->id) {
        fprintf(f, ", \"id\": ");
        json_str(f, d->id);
    }
    fprintf(f, ", \"column-origin\": 1");
    fprintf(f, ", \"locations\": [{");
    json_point(f, "caret", d->file, d->line, d->col);
    if (d->line > 0 && d->col > 0) {
        int len;
        const char *ln = d->file ? src_line(d->file, d->line, &len) : NULL;
        int end = ln ? caret_end(d, ln, len) : d->col + 1;
        fprintf(f, ", ");
        json_point(f, "finish", d->file, d->line, end - 1);
    }
    fprintf(f, "}]");
    if (d->nfixits) {
        fprintf(f, ", \"fixits\": [");
        for (int i = 0; i < d->nfixits; i++) {
            const struct fixit *x = &d->fixits[i];
            fprintf(f, "%s{", i ? ", " : "");
            json_point(f, "start", d->file, x->line, x->col);
            fprintf(f, ", ");
            json_point(f, "next", d->file, x->line, x->end_col);
            fprintf(f, ", \"string\": ");
            json_str(f, x->text);
            fputc('}', f);
        }
        fputc(']', f);
    }
    if (d->nnotes) {
        fprintf(f, ", \"children\": [\n");
        for (int i = 0; i < d->nnotes; i++) {
            render_json(f, &d->notes[i], 1);
            fprintf(f, "%s\n", i + 1 < d->nnotes ? "," : "");
        }
        fprintf(f, "%s]", pad);
    }
    fputc('}', f);
}

/* ---- fix-its an editor or the driver can apply ----------------------------
 *
 * GCC's parseable form, one line per fix-it:
 *     fix-it:"FILE":{LINE:COL-LINE:NEXT}:"TEXT"
 * and --fix, which does the edit itself. Both work from the same records,
 * so what is printed is what would be applied. */
static void render_fixits_parseable(const struct diag *d)
{
    for (int i = 0; i < d->nfixits; i++) {
        const struct fixit *x = &d->fixits[i];
        fprintf(stderr, "fix-it:\"%s\":{%d:%d-%d:%d}:\"",
                d->file ? d->file : "<embcc>", x->line, x->col, x->line,
                x->end_col);
        for (const char *p = x->text; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', stderr);
            fputc(*p, stderr);
        }
        fputs("\"\n", stderr);
    }
    for (int i = 0; i < d->nnotes; i++)
        render_fixits_parseable(&d->notes[i]);
}

/* Every fix-it of every diagnostic, flattened: (file, line, col, end, text). */
struct flat_fix { const char *file; int line, col, end_col; const char *text; };

static int collect_fixits(const struct diag *d, struct flat_fix *out, int n,
                          int cap)
{
    for (int i = 0; i < d->nfixits && n < cap; i++) {
        out[n].file = d->file;
        out[n].line = d->fixits[i].line;
        out[n].col = d->fixits[i].col;
        out[n].end_col = d->fixits[i].end_col;
        out[n].text = d->fixits[i].text;
        n++;
    }
    for (int i = 0; i < d->nnotes; i++)
        n = collect_fixits(&d->notes[i], out, n, cap);
    return n;
}

/* --fix: rewrite each file the fix-its name. Applied from the end of the
 * file backwards so earlier positions stay valid, one fix-it per line at
 * most (two edits to one line could overlap, and a wrong edit is worse
 * than none). Returns how many were applied. */
int diag_apply_fixits(void)
{
    enum { MAXFIX = 512 };
    struct flat_fix fx[MAXFIX];
    int n = 0;
    for (int i = 0; i < g_ndiag; i++)
        n = collect_fixits(&g_diags[i], fx, n, MAXFIX);
    int applied = 0;
    for (int i = 0; i < n; i++) {
        if (!fx[i].file)
            continue;
        /* the file's current text, as the diagnostics saw it */
        const char *text = NULL;
        for (int k = 0; k < g_nsrc; k++)
            if (!strcmp(g_srcs[k].file, fx[i].file)) { text = g_srcs[k].text; break; }
        if (!text)
            continue;
        /* the edits for this file, later lines first */
        struct flat_fix here[MAXFIX];
        int m = 0;
        for (int k = i; k < n; k++)
            if (fx[k].file && !strcmp(fx[k].file, fx[i].file))
                here[m++] = fx[k];
        for (int a = 0; a < m; a++)
            for (int b = a + 1; b < m; b++)
                if (here[b].line > here[a].line ||
                    (here[b].line == here[a].line && here[b].col > here[a].col)) {
                    struct flat_fix t = here[a]; here[a] = here[b]; here[b] = t;
                }
        struct sb { char *p; size_t n, cap; } buf = { NULL, 0, 0 };
        size_t tlen = strlen(text);
        buf.p = xmalloc(tlen + 4096);
        memcpy(buf.p, text, tlen + 1);
        buf.n = tlen;
        int did = 0, lastline = -1;
        for (int k = 0; k < m; k++) {
            if (here[k].line == lastline)
                continue;                  /* one edit a line */
            /* offset of the line's start */
            size_t off = 0;
            for (int ln = 1; ln < here[k].line; ln++) {
                char *nl = strchr(buf.p + off, '\n');
                if (!nl) { off = (size_t)-1; break; }
                off = (size_t)(nl - buf.p) + 1;
            }
            if (off == (size_t)-1)
                continue;
            size_t a = off + (size_t)(here[k].col - 1);
            size_t b = off + (size_t)(here[k].end_col - 1);
            if (a > buf.n || b > buf.n || b < a)
                continue;
            size_t tl = strlen(here[k].text);
            char *nbuf = xmalloc(buf.n - (b - a) + tl + 1);
            memcpy(nbuf, buf.p, a);
            memcpy(nbuf + a, here[k].text, tl);
            memcpy(nbuf + a + tl, buf.p + b, buf.n - b + 1);
            free(buf.p);
            buf.p = nbuf;
            buf.n = buf.n - (b - a) + tl;
            lastline = here[k].line;
            did++;
        }
        if (did && plat_write_file(fx[i].file, buf.p, buf.n) == 0) {
            fprintf(stderr, "embcc: %s: applied %d fix%s\n", fx[i].file,
                    did, did == 1 ? "" : "es");
            applied += did;
        }
        free(buf.p);
        /* the rest of this file's fix-its are done (the name is kept: the
         * loop below clears fx[i].file itself) */
        const char *donefile = fx[i].file;
        for (int k = i; k < n; k++)
            if (fx[k].file && !strcmp(fx[k].file, donefile))
                fx[k].file = NULL;
    }
    return applied;
}

/* ---- the flush ---- */

void diag_flush(void)
{
    if (g_flushed)
        return;
    g_flushed = 1;
    if (g_format == DIAG_JSON) {
        fprintf(stderr, "[\n");
        for (int i = 0; i < g_ndiag; i++) {
            render_json(stderr, &g_diags[i], 0);
            fprintf(stderr, "%s\n", i + 1 < g_ndiag ? "," : "");
        }
        fprintf(stderr, "]\n");
        return;
    }
    for (int i = 0; i < g_ndiag; i++) {
        render_text(&g_diags[i]);
        for (int k = 0; k < g_diags[i].nnotes; k++)
            render_text(&g_diags[i].notes[k]);
        if (g_parseable_fixits)
            render_fixits_parseable(&g_diags[i]);
    }
}

/* "compilation terminated": the line a person reads after the errors. In
 * JSON the array is the whole output, so there is nothing to add to it. */
void diag_terminated(int nerrors)
{
    diag_flush();
    if (g_format == DIAG_TEXT)
        fprintf(stderr, "embcc: compilation terminated: %d error%s\n",
                nerrors, nerrors == 1 ? "" : "s");
}

static void install_flush(void)
{
    static int done;
    if (!done) {
        done = 1;
        atexit(diag_flush);
    }
}

/* ---- warnings a -W option controls ---------------------------------------
 *
 * Each has a name, the groups it belongs to, and whether it is on. A
 * warning that is off is not recorded at all — no cost beyond the check.
 * The name is printed with the diagnostic, as GCC prints it, so the reader
 * knows what to turn off (or what to look up). */
struct warn_opt {
    const char *name;
    int on;
    int in_wall, in_wextra;
};

static struct warn_opt g_warns[] = {
    /* name                  on  -Wall -Wextra */
    { "unused-variable",      0,  1,    0 },
    { "unused-parameter",     0,  0,    1 },
    { "unused-function",      0,  1,    0 },
    { "shadow",               0,  0,    0 },
    { "sign-compare",         0,  0,    1 },
    /* GCC puts both in -Wall: a definite read of an uninitialized local is
     * a bug, and a read on some path is one often enough to be worth the
     * reader's attention. */
    { "uninitialized",        0,  1,    0 },
    { "maybe-uninitialized",  0,  1,    0 },
    /* GCC puts -Wformat in -Wall, and for the same reason: a format
     * that disagrees with its arguments reads the wrong bytes off the
     * variadic tail, which nothing else in the language catches. */
    { "format",               0,  1,    0 },
};
static const int g_nwarns = (int)(sizeof g_warns / sizeof g_warns[0]);

static struct warn_opt *warn_find(const char *name)
{
    for (int i = 0; i < g_nwarns; i++)
        if (!strcmp(g_warns[i].name, name))
            return &g_warns[i];
    return NULL;
}

int diag_warning_enabled(const char *name)
{
    struct warn_opt *w = warn_find(name);
    return w ? w->on : 0;
}

/* -Wname / -Wno-name. An unknown name is accepted and ignored: a build
 * that passes GCC's whole warning vocabulary must still compile. */
void diag_enable_warning(const char *name, int on)
{
    struct warn_opt *w = warn_find(name);
    if (w)
        w->on = on;
}

/* -Wall / -Wextra: the groups, as GCC draws them. */
void diag_enable_group(int wall, int wextra)
{
    for (int i = 0; i < g_nwarns; i++)
        if ((wall && g_warns[i].in_wall) || (wextra && g_warns[i].in_wextra))
            g_warns[i].on = 1;
}

/* The names, for --help. */
int diag_warning_count(void) { return g_nwarns; }
const char *diag_warning_name(int i) { return g_warns[i].name; }
int diag_warning_group(int i)
{
    return g_warns[i].in_wall ? 1 : g_warns[i].in_wextra ? 2 : 0;
}

/* ---- the API the front ends call ---- */

void diag_set_format(int format) { g_format = format; }
void diag_set_color(int mode)    { g_color = mode; }
void diag_set_max_errors(int n)  { g_max_errors = n; }
void diag_set_werror(int on)     { g_werror = on; }
void diag_set_no_warnings(int on){ g_no_warnings = on; }
void diag_set_parseable_fixits(int on) { g_parseable_fixits = on; }
int  diag_error_count(void)      { return g_errors; }

/* The id of the diagnostic just raised, for `embcc --explain`. */
void diag_set_id(const char *id)
{
    struct diag *d = last_diag();
    if (d)
        d->id = id;
}

void diag_range(int end_col)
{
    struct diag *d = fixit_target();
    if (d)
        d->end_col = end_col;
}

/* A fix-it for the first `find` at or after (line, from_col) in the
 * registered source: the front end knows what to replace but not where the
 * token is, and the source is right here. Returns 0 if it is not there (a
 * macro expansion, a file with no registered text), having done nothing —
 * a fix-it must never point at something it did not see. */
int diag_fixit_find(const char *file, int line, int from_col,
                    const char *find, const char *text)
{
    int len;
    const char *ln = src_line(file, line, &len);
    if (!ln || from_col < 1)
        return 0;
    int flen = (int)strlen(find);
    for (int i = from_col - 1; i + flen <= len; i++)
        if (!memcmp(ln + i, find, (size_t)flen)) {
            diag_fixit_at(file, line, i + 1, i + 1 + flen, text);
            return 1;
        }
    return 0;
}

void diag_fixit_at(const char *file, int line, int col, int end_col,
                   const char *text)
{
    struct diag *d = fixit_target();
    if (!d)
        return;
    (void)file;                      /* (a fix-it is on its diagnostic's file) */
    d->fixits = xrealloc(d->fixits, (size_t)(d->nfixits + 1) * sizeof *d->fixits);
    struct fixit *x = &d->fixits[d->nfixits++];
    x->line = line;
    x->col = col;
    x->end_col = end_col;
    x->text = xstrndup(text, strlen(text));
}

/* Where a library unwinds to instead of ending the process (util.h). */
static jmp_buf *g_fatal_boundary;

void fatal_set_boundary(void *jmp_buf_ptr)
{
    g_fatal_boundary = (jmp_buf *)jmp_buf_ptr;
}

/* Leave for the boundary if there is one; otherwise this really is the end. */
EMBCC_NORETURN static void leave(void)
{
    diag_flush();
    if (g_fatal_boundary)
        longjmp(*g_fatal_boundary, 1);
    exit(1);
}

/* -fmax-errors=N: stop once N errors are out, as GCC does. */
static void check_max_errors(void)
{
    if (g_max_errors && g_errors >= g_max_errors) {
        diag_flush();
        fprintf(stderr, "embcc: compilation terminated due to -fmax-errors=%d\n",
                g_max_errors);
        leave();
    }
}

EMBCC_NORETURN void diag_at(const char *file, int line, int col, const char *fmt, ...)
{
    install_flush();
    va_list ap;
    va_start(ap, fmt);
    new_diag(DIAG_ERROR, file, line, col, vfmt(fmt, ap));
    va_end(ap);
    leave();
}

void diag_error_at(const char *file, int line, int col, const char *fmt, ...)
{
    install_flush();
    va_list ap;
    va_start(ap, fmt);
    new_diag(DIAG_ERROR, file, line, col, vfmt(fmt, ap));
    va_end(ap);
    check_max_errors();
}

/* The same, for a front end that already has the va_list (its own
 * error function on top of this one). */
void diag_verror_at(const char *file, int line, int col, const char *fmt,
                    va_list ap)
{
    install_flush();
    new_diag(DIAG_ERROR, file, line, col, vfmt(fmt, ap));
    check_max_errors();
}

void diag_note_at(const char *file, int line, int col, const char *fmt, ...)
{
    install_flush();
    va_list ap;
    va_start(ap, fmt);
    new_diag(DIAG_NOTE, file, line, col, vfmt(fmt, ap));
    va_end(ap);
}

void diag_warn_at(const char *file, int line, int col, const char *fmt, ...)
{
    install_flush();
    if (g_no_warnings || in_system_header(file))
        return;
    va_list ap;
    va_start(ap, fmt);
    char *msg = vfmt(fmt, ap);
    va_end(ap);
    new_diag(g_werror ? DIAG_ERROR : DIAG_WARNING, file, line, col, msg);
    if (g_werror)
        check_max_errors();
}

/* A warning the option `name` controls: silent unless it is on, and
 * printed with "[-Wname]" so the reader knows which it is. */
void diag_warn_opt(const char *file, int line, int col, const char *name,
                   const char *fmt, ...)
{
    install_flush();
    if (g_no_warnings || !diag_warning_enabled(name) ||
        in_system_header(file))
        return;
    va_list ap;
    va_start(ap, fmt);
    char *msg = vfmt(fmt, ap);
    va_end(ap);
    struct diag *d = new_diag(g_werror ? DIAG_ERROR : DIAG_WARNING, file,
                              line, col, msg);
    /* the option as the reader would type it */
    char *opt = xmalloc(strlen(name) + 4);
    sprintf(opt, "-W%s", name);
    d->option = opt;
    if (g_werror)
        check_max_errors();
}

EMBCC_NORETURN void fatal_unwind(void)
{
    leave();
}

EMBCC_NORETURN void internal_error(const char *fmt, ...)
{
    install_flush();
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    char *what = vfmt(fmt, ap);
    va_end(ap);
    snprintf(buf, sizeof buf, "internal error: %s", what);
    free(what);
    /* No location: an internal error is about the compiler's state, not
     * about a place in the user's file. Naming a line would send the reader
     * to one that is very likely innocent. */
    new_diag(DIAG_ERROR, NULL, 0, 0, xstrndup(buf, strlen(buf)));
    diag_note_at(NULL, 0, 0,
                 "this is a bug in EmbCC, not in the program being compiled");
    leave();
}

EMBCC_NORETURN void diag_fatal(const char *file, int line, const char *fmt, ...)
{
    install_flush();
    va_list ap;
    va_start(ap, fmt);
    new_diag(DIAG_ERROR, file, line, 0, vfmt(fmt, ap));
    va_end(ap);
    leave();
}
