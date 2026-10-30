/* getopt, getopt_long and getopt_long_only.
 *
 * All three PERMUTE, as glibc's and newlib's getopt do: "prog file -v"
 * finds the -v, moving each option in front of the operands it skipped,
 * so a caller's operands end up in argv[optind..] either way. POSIX's
 * getopt stops at the first operand instead, and a program gets that by
 * starting its option string with '+', or its user by setting
 * POSIXLY_CORRECT. Programs were written against the permuting ones --
 * EmbLinkOS's against newlib's -- and expect it. An option string that
 * starts with '-' asks for each operand in order, as an option whose
 * code is 1 and whose argument is the operand.
 *
 * The diagnostics are glibc's words, so a test that compares a program's
 * stderr sees the same line here as there.
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../internal/getopt-r.h"

__attribute__((weak)) char *optarg;
__attribute__((weak)) int optind = 1, opterr = 1, optopt, optreset;

static int optpos;      /* where in argv[optind] the next short option is */
static int nonopt_first = 1, nonopt_last = 1;  /* the operands passed over */

static void say_short(const char *prog, const char *what, int c)
{
    fprintf(stderr, "%s: %s -- '%c'\n", prog, what, c);
}

/* s->ind = 0 is a full reset: glibc's convention, and the only way a
 * program that parses two argument vectors can ask for one */
static void restart(struct __getopt_state *s)
{
    if (s->ind == 0 || s->reset) {
        s->reset = 0;
        s->pos = 0;
        s->ind = 1;
        s->first = s->last = 1;
    }
}

/* The option string without its '+' or '-' mode character. */
static const char *spec_of(const char *opts)
{
    return opts[0] == '+' || opts[0] == '-' ? opts + 1 : opts;
}

/* One short option, POSIX's way: no permuting, which the callers do. */
static int short_opt(struct __getopt_state *s, int argc, char *const argv[], const char *opts)
{
    restart(s);
    s->arg = NULL;
    if (s->ind >= argc || !argv[s->ind])
        return -1;
    char *arg = argv[s->ind];
    if (s->pos == 0) {
        if (arg[0] != '-' || !arg[1]) {
            if (opts[0] != '-')
                return -1;
            s->arg = arg;              /* '-' in opts: operands in order */
            s->ind++;
            return 1;
        }
        if (arg[1] == '-' && !arg[2]) {
            s->ind++;                  /* "--" ends the options */
            return -1;
        }
        s->pos = 1;
    }
    const char *spec = spec_of(opts);
    int quiet = spec[0] == ':';        /* report ':' rather than print */
    int c = (unsigned char)arg[s->pos++];
    const char *at = c == ':' ? NULL : strchr(spec, c);
    char *rest = arg + s->pos;         /* what follows c in this argument */

    if (!at || !at[1] || at[1] != ':') {
        if (!*rest) {
            s->ind++;
            s->pos = 0;
        }
        if (at)
            return c;
        s->opt = c;
        if (s->err && !quiet)
            say_short(argv[0], "invalid option", c);
        return '?';
    }

    /* c takes an argument: the rest of this one, or the next one -- or,
     * for "c::", GNU's optional argument, only the rest of this one */
    s->ind++;
    s->pos = 0;
    if (*rest)
        s->arg = rest;
    else if (at[2] == ':')
        s->arg = NULL;
    else if (s->ind < argc && argv[s->ind])
        s->arg = argv[s->ind++];
    else {
        s->opt = c;
        if (quiet)
            return ':';
        if (s->err)
            say_short(argv[0], "option requires an argument", c);
        return '?';
    }
    return c;
}

/* Move argv[src] to argv[dest], shifting the ones between up by one. The
 * strings are not touched, only the vector; POSIX's prototype says
 * `char *const argv[]`, and every getopt that permutes casts that away. */
static void move_down(char *const *argv, int dest, int src)
{
    char **av = (char **)argv;
    char *tmp = av[src];
    for (int i = src; i > dest; i--)
        av[i] = av[i - 1];
    av[dest] = tmp;
}

static int long_core(struct __getopt_state *s, int argc, char *const argv[], const char *opts,
                     const struct option *lo, int *index, int only)
{
    char *arg = argv[s->ind];
    int dashes = arg[1] == '-' ? 2 : 1;
    if (!lo || s->pos != 0 || arg[0] != '-' ||
        !(dashes == 2 ? arg[2] != 0 : only && arg[1] != 0))
        return short_opt(s, argc, argv, opts);

    const char *spec = spec_of(opts);
    int quiet = spec[0] == ':';
    const char *name = arg + dashes;
    size_t n = strcspn(name, "=");
    int match = -1, count = 0;
    for (int i = 0; lo[i].name; i++) {
        if (strncmp(lo[i].name, name, n) != 0)
            continue;
        if (lo[i].name[n] == 0) {      /* exact: no other can compete */
            match = i;
            count = 1;
            break;
        }
        /* GNU's rule: prefixes of several names are one match when the
         * options they name are the same option */
        if (match < 0 || lo[i].has_arg != lo[match].has_arg ||
            lo[i].flag != lo[match].flag || lo[i].val != lo[match].val)
            count++;
        if (match < 0)
            match = i;
    }
    /* -abc under getopt_long_only that names no long option, but whose
     * first letter is a short one: the short options it spells */
    if (count == 0 && dashes == 1 && strchr(spec, arg[1]))
        return short_opt(s, argc, argv, opts);

    const char *dd = dashes == 2 ? "--" : "-";
    s->ind++;
    if (count != 1) {
        s->opt = 0;
        if (s->err && !quiet && !count)
            fprintf(stderr, "%s: unrecognized option '%s%s'\n", argv[0], dd, name);
        else if (s->err && !quiet) {
            /* the first match, then each that names another option */
            flockfile(stderr);
            fprintf(stderr, "%s: option '%s%s' is ambiguous; possibilities:", argv[0], dd,
                    name);
            for (int i = 0; lo[i].name; i++)
                if (!strncmp(lo[i].name, name, n) &&
                    (i == match || lo[i].has_arg != lo[match].has_arg ||
                     lo[i].flag != lo[match].flag || lo[i].val != lo[match].val))
                    fprintf(stderr, " '%s%s'", dd, lo[i].name);
            fputc('\n', stderr);
            funlockfile(stderr);
        }
        return '?';
    }
    const struct option *o = &lo[match];
    s->arg = NULL;
    if (name[n] == '=') {
        if (o->has_arg == no_argument) {
            s->opt = o->val;
            if (s->err && !quiet)
                fprintf(stderr, "%s: option '%s%s' doesn't allow an argument\n", argv[0],
                        dd, o->name);
            return '?';
        }
        s->arg = (char *)name + n + 1;
    } else if (o->has_arg == required_argument) {
        if (s->ind >= argc || !argv[s->ind]) {
            s->opt = o->val;
            if (quiet)
                return ':';
            if (s->err)
                fprintf(stderr, "%s: option '%s%s' requires an argument\n", argv[0], dd,
                        o->name);
            return '?';
        }
        s->arg = argv[s->ind++];
    }
    if (index)
        *index = match;
    if (o->flag) {
        *o->flag = o->val;
        return 0;
    }
    return o->val;
}

static int at_end(int argc, char *const argv[], int i)
{
    return i >= argc || !argv[i];
}

/* Swap the operands [a, b) and the options [b, c) after them, each block
 * keeping its order. */
static void rotate(char *const *argv, int a, int b, int c)
{
    for (int i = b; i < c; i++)
        move_down(argv, a + (i - b), i);
}

/* Permuting is lazy, as GNU's is: operands passed over are remembered
 * (first to last), and the options taken after them go in front of them
 * at the start of the next call -- so while a caller looks at an option,
 * optind is where that option ended in the argv it was given, as glibc
 * has it, and argp's state->next with it. */
int __getopt_r(struct __getopt_state *s, int argc, char *const argv[], const char *opts,
               const struct option *lo, int *index, int only)
{
    restart(s);
    if (s->pos == 0 && opts[0] != '+' && opts[0] != '-' && !getenv("POSIXLY_CORRECT")) {
        if (s->last > s->ind)          /* the caller moved optind back */
            s->last = s->ind;
        if (s->first > s->ind)
            s->first = s->ind;
        if (s->first != s->last && s->last != s->ind) {
            rotate(argv, s->first, s->last, s->ind);
            s->first += s->ind - s->last;
        } else if (s->last != s->ind) {
            s->first = s->ind;
        }
        while (!at_end(argc, argv, s->ind) && !(argv[s->ind][0] == '-' && argv[s->ind][1]))
            s->ind++;
        s->last = s->ind;
        if (!at_end(argc, argv, s->ind) && !strcmp(argv[s->ind], "--")) {
            /* "--" goes in front of the operands, as an option would, and
             * everything after it is an operand */
            s->ind++;
            if (s->first != s->last && s->last != s->ind) {
                rotate(argv, s->first, s->last, s->ind);
                s->first += s->ind - s->last;
            } else if (s->first == s->last) {
                s->first = s->ind;
            }
            s->last = s->ind = argc;
        }
        if (at_end(argc, argv, s->ind)) {
            if (s->first != s->last)
                s->ind = s->first;     /* back to the operands */
            return -1;
        }
    } else if (at_end(argc, argv, s->ind)) {
        return -1;
    }
    return long_core(s, argc, argv, opts, lo, index, only);
}

/* the public calls: getopt's globals in and out of a state */
static int global(int argc, char *const argv[], const char *opts,
                  const struct option *lo, int *index, int only)
{
    struct __getopt_state s = { optind, opterr, optopt, optreset, optpos, optarg,
                                nonopt_first, nonopt_last };
    int r = __getopt_r(&s, argc, argv, opts, lo, index, only);
    nonopt_first = s.first;
    nonopt_last = s.last;
    optind = s.ind;
    optopt = s.opt;
    optreset = s.reset;
    optpos = s.pos;
    optarg = s.arg;
    return r;
}

__attribute__((weak)) int getopt(int argc, char *const argv[], const char *opts)
{
    return global(argc, argv, opts, NULL, NULL, 0);
}

__attribute__((weak)) int getopt_long(int argc, char *const argv[], const char *opts,
                const struct option *lo, int *index)
{
    return global(argc, argv, opts, lo, index, 0);
}

__attribute__((weak)) int getopt_long_only(int argc, char *const argv[], const char *opts,
                     const struct option *lo, int *index)
{
    return global(argc, argv, opts, lo, index, 1);
}
