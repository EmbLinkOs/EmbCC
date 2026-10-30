/* <argp.h> -- GNU's argument parser (src/posix/argp.c): a program
 * describes its options once, in tables that nest (a library's options
 * as a child of the program's), and argp_parse parses argv by them and
 * writes --help and --usage from them. The structures, keys, flags and
 * numbers are glibc's, so a program written against glibc's argp builds
 * unchanged, and its help reads the same. */
#ifndef _ARGP_H
#define _ARGP_H

#include <errno.h>
#include <stdio.h>
#include <limits.h>

#ifndef __error_t_defined
#define __error_t_defined 1
typedef int error_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct argp_option {
    const char *name;    /* the long option, or null */
    int key;             /* the short option when a printable char; the
                          * parser's key either way */
    const char *arg;     /* the argument's name in the help, or null */
    int flags;           /* OPTION_* */
    const char *doc;     /* what it does -- or, with name and key 0, a
                          * group header */
    int group;           /* where it sorts in the help; 0: as before it */
};

#define OPTION_ARG_OPTIONAL 0x1   /* the argument may be left out */
#define OPTION_HIDDEN       0x2   /* not in the help */
#define OPTION_ALIAS        0x4   /* another name for the option before */
#define OPTION_DOC          0x8   /* not an option: help text in its place */
#define OPTION_NO_USAGE     0x10  /* not in the usage line */
#define OPTION_NO_TRANS     0x20  /* the doc is not to be translated */

struct argp;
struct argp_state;
struct argp_child;

typedef error_t (*argp_parser_t)(int key, char *arg, struct argp_state *state);

/* a parser's answer for a key it does not handle */
#define ARGP_ERR_UNKNOWN E2BIG

/* glibc's numbers; where int is 16 bits (AVR), the top of its range */
#if INT_MAX > 0x1000007
#define __ARGP_KEY(n)  (0x1000000 + (n))
#define __ARGP_HKEY(n) (0x2000000 + (n))
#else
#define __ARGP_KEY(n)  (0x7f00 + (n))
#define __ARGP_HKEY(n) (0x7e00 + (n))
#endif

#define ARGP_KEY_ARG     0              /* an operand, in arg */
#define ARGP_KEY_ARGS    __ARGP_KEY(6)  /* the operands from state->next on */
#define ARGP_KEY_END     __ARGP_KEY(1)  /* no more arguments */
#define ARGP_KEY_NO_ARGS __ARGP_KEY(2)  /* this parser was given no operands */
#define ARGP_KEY_INIT    __ARGP_KEY(3)  /* before anything is parsed */
#define ARGP_KEY_FINI    __ARGP_KEY(7)  /* the very last call */
#define ARGP_KEY_SUCCESS __ARGP_KEY(4)  /* parsing succeeded */
#define ARGP_KEY_ERROR   __ARGP_KEY(5)  /* parsing failed */

struct argp {
    const struct argp_option *options;   /* ended by an all-zero entry */
    argp_parser_t parser;
    const char *args_doc;    /* the operands, for the usage line; lines
                              * after a '\n' are alternative usages */
    const char *doc;         /* the help's text: before a '\v', above the
                              * options; after it, below */
    const struct argp_child *children;   /* ended by a null argp */
    char *(*help_filter)(int key, const char *text, void *input);
    const char *argp_domain; /* the translation domain */
};

/* help_filter's keys besides an option's own */
#define ARGP_KEY_HELP_PRE_DOC       __ARGP_HKEY(1)
#define ARGP_KEY_HELP_POST_DOC      __ARGP_HKEY(2)
#define ARGP_KEY_HELP_HEADER        __ARGP_HKEY(3)
#define ARGP_KEY_HELP_EXTRA         __ARGP_HKEY(4)
#define ARGP_KEY_HELP_DUP_ARGS_NOTE __ARGP_HKEY(5)
#define ARGP_KEY_HELP_ARGS_DOC      __ARGP_HKEY(6)

struct argp_child {
    const struct argp *argp;
    int flags;
    const char *header;      /* its options' heading in the help */
    int group;               /* where they sort */
};

struct argp_state {
    const struct argp *root_argp;
    int argc;
    char **argv;
    int next;                /* the next argument to parse */
    unsigned flags;          /* argp_parse's */
    unsigned arg_num;        /* the operands this parser has had */
    int quoted;              /* where the arguments after "--" start, or 0 */
    void *input;             /* this parser's input */
    void **child_inputs;     /* its children's, set at ARGP_KEY_INIT */
    void *hook;              /* this parser's own */
    char *name;              /* the program's name, for messages */
    FILE *err_stream;
    FILE *out_stream;
    void *pstate;
};

#define ARGP_PARSE_ARGV0 0x01   /* argv[0] is an argument too */
#define ARGP_NO_ERRS     0x02   /* no messages, no exits for errors */
#define ARGP_NO_ARGS     0x04   /* operands are not the parsers' */
#define ARGP_IN_ORDER    0x08   /* operands as they come, not after */
#define ARGP_NO_HELP     0x10   /* no --help, --usage */
#define ARGP_NO_EXIT     0x20   /* no exit, for errors or for --help */
#define ARGP_LONG_ONLY   0x40   /* -name is a long option too */
#define ARGP_SILENT      (ARGP_NO_EXIT | ARGP_NO_ERRS | ARGP_NO_HELP)

error_t argp_parse(const struct argp *__restrict argp, int argc, char **__restrict argv,
                   unsigned flags, int *__restrict arg_index, void *__restrict input);

/* the program defines these, for --version and the help's last line */
extern const char *argp_program_version;
extern void (*argp_program_version_hook)(FILE *__restrict stream,
                                         struct argp_state *__restrict state);
extern const char *argp_program_bug_address;
/* the exit status for a usage error: EX_USAGE */
extern error_t argp_err_exit_status;

#define ARGP_HELP_USAGE      0x01   /* the usage line, with the options */
#define ARGP_HELP_SHORT_USAGE 0x02  /* the usage line, [OPTION...] */
#define ARGP_HELP_SEE        0x04   /* "Try ... --help" */
#define ARGP_HELP_LONG       0x08   /* the options, each with its doc */
#define ARGP_HELP_PRE_DOC    0x10
#define ARGP_HELP_POST_DOC   0x20
#define ARGP_HELP_DOC        (ARGP_HELP_PRE_DOC | ARGP_HELP_POST_DOC)
#define ARGP_HELP_BUG_ADDR   0x40
#define ARGP_HELP_LONG_ONLY  0x80
#define ARGP_HELP_EXIT_ERR   0x100
#define ARGP_HELP_EXIT_OK    0x200
#define ARGP_HELP_STD_ERR    (ARGP_HELP_SEE | ARGP_HELP_EXIT_ERR)
#define ARGP_HELP_STD_USAGE  (ARGP_HELP_SHORT_USAGE | ARGP_HELP_SEE | ARGP_HELP_EXIT_ERR)
#define ARGP_HELP_STD_HELP   (ARGP_HELP_SHORT_USAGE | ARGP_HELP_LONG | ARGP_HELP_EXIT_OK | \
                              ARGP_HELP_DOC | ARGP_HELP_BUG_ADDR)

void argp_help(const struct argp *__restrict argp, FILE *__restrict stream,
               unsigned flags, char *__restrict name);
void argp_state_help(const struct argp_state *__restrict state, FILE *__restrict stream,
                     unsigned flags);
void argp_usage(const struct argp_state *state);
void argp_error(const struct argp_state *__restrict state, const char *__restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
void argp_failure(const struct argp_state *__restrict state, int status, int errnum,
                  const char *__restrict fmt, ...)
    __attribute__((format(printf, 4, 5)));

int _option_is_short(const struct argp_option *opt);
int _option_is_end(const struct argp_option *opt);
void *_argp_input(const struct argp *__restrict argp,
                  const struct argp_state *__restrict state);

#define __option_is_short _option_is_short
#define __option_is_end   _option_is_end
#define __argp_input      _argp_input

#ifdef __cplusplus
}
#endif

#endif
