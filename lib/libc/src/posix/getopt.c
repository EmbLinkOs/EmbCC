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

char *optarg;
int optind = 1, opterr = 1, optopt, optreset;

static int optpos;      /* where in argv[optind] the next short option is */

static void say_short(const char *prog, const char *what, int c)
{
    fprintf(stderr, "%s: %s -- '%c'\n", prog, what, c);
}

static void say_long(const char *prog, const char *what, const char *dashes,
                     const char *name, size_t n)
{
    fprintf(stderr, "%s: %s '%s%.*s'\n", prog, what, dashes, (int)n, name);
}

/* optind = 0 is a full reset: glibc's convention, and the only way a
 * program that parses two argument vectors can ask for one */
static void restart(void)
{
    if (optind == 0 || optreset) {
        optreset = 0;
        optpos = 0;
        optind = 1;
    }
}

/* The option string without its '+' or '-' mode character. */
static const char *spec_of(const char *opts)
{
    return opts[0] == '+' || opts[0] == '-' ? opts + 1 : opts;
}

/* One short option, POSIX's way: no permuting, which the callers do. */
static int short_opt(int argc, char *const argv[], const char *opts)
{
    restart();
    optarg = NULL;
    if (optind >= argc || !argv[optind])
        return -1;
    char *arg = argv[optind];
    if (optpos == 0) {
        if (arg[0] != '-' || !arg[1]) {
            if (opts[0] != '-')
                return -1;
            optarg = arg;              /* '-' in opts: operands in order */
            optind++;
            return 1;
        }
        if (arg[1] == '-' && !arg[2]) {
            optind++;                  /* "--" ends the options */
            return -1;
        }
        optpos = 1;
    }
    const char *spec = spec_of(opts);
    int quiet = spec[0] == ':';        /* report ':' rather than print */
    int c = (unsigned char)arg[optpos++];
    const char *at = c == ':' ? NULL : strchr(spec, c);
    char *rest = arg + optpos;         /* what follows c in this argument */

    if (!at || !at[1] || at[1] != ':') {
        if (!*rest) {
            optind++;
            optpos = 0;
        }
        if (at)
            return c;
        optopt = c;
        if (opterr && !quiet)
            say_short(argv[0], "invalid option", c);
        return '?';
    }

    /* c takes an argument: the rest of this one, or the next one -- or,
     * for "c::", GNU's optional argument, only the rest of this one */
    optind++;
    optpos = 0;
    if (*rest)
        optarg = rest;
    else if (at[2] == ':')
        optarg = NULL;
    else if (optind < argc && argv[optind])
        optarg = argv[optind++];
    else {
        optopt = c;
        if (quiet)
            return ':';
        if (opterr)
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

static int long_core(int argc, char *const argv[], const char *opts,
                     const struct option *lo, int *index, int only)
{
    char *arg = argv[optind];
    int dashes = arg[1] == '-' ? 2 : 1;
    if (!lo || optpos != 0 || arg[0] != '-' ||
        !(dashes == 2 ? arg[2] != 0 : only && arg[1] != 0))
        return short_opt(argc, argv, opts);

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
        return short_opt(argc, argv, opts);

    const char *dd = dashes == 2 ? "--" : "-";
    optind++;
    if (count != 1) {
        optopt = 0;
        if (opterr && !quiet)
            say_long(argv[0], count ? "option is ambiguous;" :
                     "unrecognized option", dd, name, strlen(name));
        return '?';
    }
    const struct option *o = &lo[match];
    optarg = NULL;
    if (name[n] == '=') {
        if (o->has_arg == no_argument) {
            optopt = o->val;
            if (opterr && !quiet)
                say_long(argv[0], "option doesn't allow an argument", dd,
                         o->name, strlen(o->name));
            return '?';
        }
        optarg = (char *)name + n + 1;
    } else if (o->has_arg == required_argument) {
        if (optind >= argc || !argv[optind]) {
            optopt = o->val;
            if (quiet)
                return ':';
            if (opterr)
                say_long(argv[0], "option requires an argument", dd,
                         o->name, strlen(o->name));
            return '?';
        }
        optarg = argv[optind++];
    }
    if (index)
        *index = match;
    if (o->flag) {
        *o->flag = o->val;
        return 0;
    }
    return o->val;
}

static int long_permuting(int argc, char *const argv[], const char *opts,
                          const struct option *lo, int *index, int only)
{
    restart();
    if (optind >= argc || !argv[optind])
        return -1;
    int skipped = optind;
    /* Mid-cluster too: the search finds the same "-abc" again, since it
     * starts with '-', and optpos still says where in it to go on. */
    if (opts[0] != '+' && opts[0] != '-' && !getenv("POSIXLY_CORRECT")) {
        int i = optind;
        for (;; i++) {
            if (i >= argc || !argv[i])
                return -1;             /* operands only, all left in place */
            if (argv[i][0] == '-' && argv[i][1])
                break;
        }
        optind = i;
    }
    int resumed = optind;
    int r = long_core(argc, argv, opts, lo, index, only);
    /* The arguments consumed come in front of the operands passed over.
     * In the middle of "-abc" none has been, and optind goes back to the
     * first operand -- which the next call's search passes over again. */
    if (resumed > skipped) {
        int taken = optind - resumed;
        for (int i = 0; i < taken; i++)
            move_down(argv, skipped, optind - 1);
        optind = skipped + taken;
    }
    return r;
}

int getopt(int argc, char *const argv[], const char *opts)
{
    return long_permuting(argc, argv, opts, NULL, NULL, 0);
}

int getopt_long(int argc, char *const argv[], const char *opts,
                const struct option *lo, int *index)
{
    return long_permuting(argc, argv, opts, lo, index, 0);
}

int getopt_long_only(int argc, char *const argv[], const char *opts,
                     const struct option *lo, int *index)
{
    return long_permuting(argc, argv, opts, lo, index, 1);
}
