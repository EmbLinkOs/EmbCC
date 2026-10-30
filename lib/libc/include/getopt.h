/* <getopt.h> -- getopt, and GNU's getopt_long and getopt_long_only
 * (src/posix/getopt.c). <unistd.h> declares getopt and its variables
 * too; this header adds the long forms. */
#ifndef _GETOPT_H
#define _GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

extern char *optarg;
extern int optind, opterr, optopt;
/* BSD's way to start over; setting optind to 0 does the same, as glibc */
extern int optreset;

int getopt(int argc, char *const argv[], const char *opts);

struct option {
    const char *name;
    int has_arg;           /* no_argument, required_argument, optional_ */
    int *flag;             /* non-null: *flag = val and return 0 */
    int val;
};

#define no_argument       0
#define required_argument 1
#define optional_argument 2

int getopt_long(int argc, char *const argv[], const char *opts,
                const struct option *longopts, int *index);
int getopt_long_only(int argc, char *const argv[], const char *opts,
                     const struct option *longopts, int *index);

#ifdef __cplusplus
}
#endif

#endif
