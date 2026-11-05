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

/* ---- the boards ----------------------------------------------------------
 *
 * --board=NAME stands for the options a board's program is compiled with
 * (its target, its part, its clock as F_CPU, its runtime's flags) and for
 * what the link adds: the startup and the board's runtime (lib/boards,
 * built beside the target's library by tools/build-rt.sh), and the
 * board's linker script, or the part's. <board.h> is the board's
 * (include/boards/DIR), the same names on every board. */
static const struct embcc_board boards[] = {
    { "uno", "Arduino Uno (ATmega328P, 16 MHz, Optiboot)", "avr", "atmega328p",
      "-DF_CPU=16000000UL", "uno", "board-uno.o", NULL,
      "--stk500 serial:PORT (115200)" },
    { "nano", "Arduino Nano (ATmega328P, 16 MHz, Optiboot)", "avr", "atmega328p",
      "-DF_CPU=16000000UL -DBOARD_NANO=1", "uno", "board-uno.o", NULL,
      "--stk500 serial:PORT (115200; serial:PORT@57600 for the old bootloader)" },
    { "esp32-devkit", "ESP32 DevKit (ESP32-WROOM-32, 40 MHz from the ROM, 4 MiB flash)",
      "xtensa-esp32-elf", NULL, "-mtext-section-literals", "esp32-devkit",
      "esp32-crt0.o esp32-start.o board-esp32-devkit.o", "esp32.ld",
      "--esp serial:PORT (460800)" },
};
#define NBOARDS ((int)(sizeof boards / sizeof boards[0]))
static const struct embcc_board *g_board;

const struct embcc_board *opts_board(void) { return g_board; }

void opts_print_boards(FILE *out)
{
    fprintf(out, "# Board reference\n\nEvery board `--board=` accepts, generated from EmbCC's "
                 "board table (`src/driver/options.c`) by `embcc --reference=boards`. "
                 "[Boards](boards.md) is the guide: what a board's program gets, `<board.h>`, "
                 "and putting it on the board.\n\n");
    fprintf(out, "| Board | What it is | Target | Programmed with |\n|---|---|---|---|\n");
    for (int k = 0; k < NBOARDS; k++)
        fprintf(out, "| `%s` | %s | `%s`%s%s%s | `embflash %s` |\n", boards[k].name,
                boards[k].title, boards[k].target, boards[k].mcu ? " `--mcu=" : "",
                boards[k].mcu ? boards[k].mcu : "", boards[k].mcu ? "`" : "", boards[k].flash);
}

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

/* What the user typed for a rewritten option, for messages (opt_typed,
 * util.c): "--cpu cortex-m4" for "-mcpu=cortex-m4". */
static void remember(const char *internal, const char *spelling,
                     const char *val, int sep)
{
    size_t n = strlen(spelling) + (val ? strlen(val) : 0) + 2;
    char *t = xmalloc(n);
    if (!val || !*val)
        snprintf(t, n, "%s", spelling);
    else if (sep)
        snprintf(t, n, "%.*s %s", (int)strlen(spelling) - 1, spelling, val);
    else
        snprintf(t, n, "%s%s", spelling, val);
    opt_remember_typed(internal, t);
    free(t);
}

/* argv[i] is --board=NAME or --board NAME: the board (i past its value), or
 * NULL when it is not one */
static const char *board_arg(int argc, char **argv, int *i)
{
    const char *a = argv[*i];
    if (strncmp(a, "--board=", 8) == 0)
        return a + 8;
    if (strcmp(a, "--board") == 0 && *i + 1 < argc)
        return argv[++*i];
    return NULL;
}

int opts_normalize(int *argc, char ***argv)
{
    struct strvec out = { 0, 0, 0 };
    int changed = 0;
    for (int i = 1; i < *argc; i++) {
        int j = i;
        const char *b = board_arg(*argc, *argv, &j);
        if (!b)
            continue;
        const struct embcc_board *bd = NULL;
        for (int k = 0; k < NBOARDS; k++)
            if (strcmp(b, boards[k].name) == 0)
                bd = &boards[k];
        if (!bd) {
            fprintf(stderr, "embcc: error: --board=%s: not a board EmbCC knows; it knows:\n", b);
            for (int k = 0; k < NBOARDS; k++)
                fprintf(stderr, "embcc:   %-14s %s\n", boards[k].name, boards[k].title);
            return -1;
        }
        if (g_board && g_board != bd) {
            fprintf(stderr, "embcc: error: --board=%s and --board=%s: one board at a time\n",
                    g_board->name, bd->name);
            return -1;
        }
        for (int k = 1; k < *argc; k++)
            if (strncmp((*argv)[k], "--target=", 9) == 0 &&
                strcmp((*argv)[k] + 9, bd->target) != 0) {
                fprintf(stderr, "embcc: error: --board=%s is a %s board, and %s names "
                        "another target\n", bd->name, bd->target, (*argv)[k]);
                return -1;
            }
        g_board = bd;
        i = j;
    }
    for (int i = 0; i < *argc; i++) {
        const char *a = (*argv)[i];
        if (i > 0 && g_board) {
            int j = i;
            if (board_arg(*argc, *argv, &j)) {
                /* what the board stands for, in its place */
                char t[160];
                snprintf(t, sizeof t, "--target=%s", g_board->target);
                expand(&out, t, "");
                if (g_board->mcu) {
                    snprintf(t, sizeof t, "-mmcu=%s", g_board->mcu);
                    expand(&out, t, "");
                }
                expand(&out, g_board->flags, "");
                i = j;
                changed = 1;
                continue;
            }
        }
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
        int first = out.n;
        expand(&out, o->gcc, val ? val : "");
        remember(out.v[first], o->spelling, val, sep);
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
        "(`src/driver/options.def`) by `embcc --reference=options`, so it "
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

/* ---- the man page ----------------------------------------------------- */

/* The bytes on the man page's current input line, which roff() keeps
 * under 80 (mandoc's lint reports a longer one): each caller sets it to
 * what it has written of the line before. */
static int roff_col;

static void roff_put(FILE *out, const char *s)
{
    fputs(s, out);
    roff_col += (int)strlen(s);
}

/* Text as roff reads it: a backslash starts an escape, a '-' is a hyphen
 * unless escaped (and an option's dashes must stay minus signs), and a
 * line starting with '.' or '\'' would be a request. Markdown's `code`
 * becomes bold. A long text is broken at a space into several input
 * lines, which roff fills back into one paragraph. */
/* The bytes the word at s takes on the line, escaped as roff() writes it */
static int roff_word_len(const char *s, const char *arg)
{
    int n = 0;
    for (; *s && *s != ' '; s++)
        n += *s == '-' || *s == '\\' ? 2 : *s == '`' ? 3
           : *s == '%' && arg ? (int)strlen(arg) + 8 : 1;
    return n;
}

static void roff(FILE *out, const char *s, const char *arg)
{
    int code = 0;
    if (*s == '.' || *s == '\'')
        roff_put(out, "\\&");
    for (; *s; s++) {
        if (*s == ' ' && s[1] &&
            (roff_col >= 64 || roff_col + 1 + roff_word_len(s + 1, arg) > 79)) {
            fputc('\n', out);
            roff_col = 0;
            if (s[1] == '.' || s[1] == '\'')
                roff_put(out, "\\&");
        } else if (*s == '\\')
            roff_put(out, "\\e");
        else if (*s == '-')
            roff_put(out, "\\-");
        else if (*s == '`') {
            roff_put(out, code ? "\\fR" : "\\fB");
            code = !code;
        } else if (*s == '%' && arg)
            roff(out, arg, NULL);
        else {
            fputc(*s, out);
            roff_col++;
        }
    }
    if (code)
        roff_put(out, "\\fR");
}

void opts_print_man(FILE *out, const char *version, const char *date)
{
    fprintf(out,
        ".\\\" Generated by `embcc --reference=man` from src/driver/options.def.\n"
        ".\\\" Do not edit: change the table and run `make reference`.\n"
        ".TH EMBCC 1 \"%s\" \"EmbCC %s\" \"EmbCC\"\n"
        ".SH NAME\n"
        "embcc \\- the EmbCC C and C++ compiler for embedded targets\n"
        ".SH SYNOPSIS\n"
        ".B embcc\n"
        "[\\fB\\-c\\fR | \\fB\\-S\\fR | \\fB\\-E\\fR]\n"
        "[\\fIoption\\fR ...]\n"
        "\\fIsource\\fR ...\n"
        "[\\fB\\-o\\fR \\fIfile\\fR]\n"
        ".br\n"
        ".B embcc\n"
        "\\fIobject\\fR ... [\\fB\\-l\\fIname\\fR ...]\n"
        "[\\fB\\-\\-link\\-script=\\fIfile\\fR]\n"
        "\\fB\\-o\\fR \\fIimage\\fR\n"
        ".br\n"
        ".B embcc inspect\n"
        "\\fIstage\\fR \\fIsource\\fR\n"
        ".SH DESCRIPTION\n"
        ".B embcc\n"
        "compiles C and C++ for microcontrollers and operating systems:\n"
        "ARM Cortex\\-M and Cortex\\-A, RISC\\-V, AVR, MIPS, Xtensa,\n"
        "PowerPC, SPARC, ColdFire, TriCore, RX, LoongArch, x86\\-64 and\n"
        "AArch64. It preprocesses, compiles, assembles and links in one\n"
        "program, and needs no other toolchain.\n"
        ".PP\n"
        "Options are EmbCC's own spellings: POSIX's letters, then words.\n"
        "Where GCC spells an option differently, its spelling is accepted\n"
        "too and is listed under the option, so a build written for GCC\n"
        "builds unchanged. An option that takes a value takes it after\n"
        "an \\fB=\\fR or as the next argument.\n", date, version);
    for (int g = 0; g < G_COUNT; g++) {
        fputs(".SH ", out);
        for (const char *t = group_title[g]; *t; t++)
            fputc(*t >= 'a' && *t <= 'z' ? *t - 'a' + 'A' : *t, out);
        fputc('\n', out);
        for (int k = 0; k < NOPTS; k++) {
            const struct opt *o = &opts[k];
            if (o->group != (enum opt_group)g || !o->spelling)
                continue;
            fputs(".TP\n\\fB", out);
            roff_col = 3;
            roff(out, o->spelling, NULL);
            fputs("\\fR", out);
            if (o->arg) {
                fputs(takes_value(o) ? "\\fI" : " \\fI", out);
                roff(out, o->arg, NULL);
                fputs("\\fR", out);
            }
            fputc('\n', out);
            roff_col = 0;
            roff(out, o->help, NULL);
            fputc('\n', out);
            if (o->gcc) {
                fputs(".br\nGCC: \\fB", out);
                roff_col = 8;
                roff(out, o->gcc, o->arg ? o->arg : "VALUE");
                fputs("\\fR\n", out);
            }
        }
    }
    fputs(".SH GCC SPELLINGS WITH NO EMBCC NAME\n"
          "GCC flags EmbCC reads, because they change nothing here or so\n"
          "as to refuse them by name.\n", out);
    for (int k = 0; k < NOPTS; k++)
        if (!opts[k].spelling) {
            fputs(".TP\n\\fB", out);
            roff_col = 3;
            roff(out, opts[k].gcc, NULL);
            fputs("\\fR\n", out);
            roff_col = 0;
            roff(out, opts[k].help, NULL);
            fputc('\n', out);
        }
    fputs(".SH SEE ALSO\n"
          "\\fBembcc \\-\\-reference=warnings\\fR and\n"
          "\\fBembcc \\-\\-reference=targets\\fR print every warning and\n"
          "every target. The manual is at\n"
          ".UR https://emblinkos.github.io/EmbCC/\n.UE\n"
          "and in \\fIdocs/\\fR in the source.\n", out);
}
