/* getopt's state, for a caller that must not touch the program's optind
 * and the rest: argp parses with one of its own, as glibc's does. A
 * state starting with ind 0 starts afresh. */
#ifndef EMBLIBC_GETOPT_R_H
#define EMBLIBC_GETOPT_R_H

#include <getopt.h>

struct __getopt_state {
    int ind, err, opt, reset;
    int pos;                 /* where in argv[ind] the next short option is */
    char *arg;
    int first, last;         /* the operands passed over, not yet moved */
};

/* getopt_long (only: getopt_long_only) on the state s */
int __getopt_r(struct __getopt_state *s, int argc, char *const argv[], const char *opts,
               const struct option *lo, int *index, int only);

#endif
