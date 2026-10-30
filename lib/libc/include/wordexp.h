/* <wordexp.h> -- a string expanded as the shell would expand a command's
 * words: tildes, parameters, command substitution, arithmetic, field
 * splitting, pathname expansion and quote removal. POSIX's wordexp and
 * wordfree, with glibc's numbers. */
#ifndef _WORDEXP_H
#define _WORDEXP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t we_wordc;           /* how many words */
    char **we_wordv;           /* we_offs nulls, the words, a null */
    size_t we_offs;            /* with WRDE_DOOFFS, how many nulls first */
} wordexp_t;

#define WRDE_DOOFFS  (1 << 0)  /* we_offs nulls before the words */
#define WRDE_APPEND  (1 << 1)  /* after the words of a previous call */
#define WRDE_NOCMD   (1 << 2)  /* a command substitution is an error */
#define WRDE_REUSE   (1 << 3)  /* free a previous call's words first */
#define WRDE_SHOWERR (1 << 4)  /* commands' standard error is not discarded */
#define WRDE_UNDEF   (1 << 5)  /* an unset parameter is an error */

enum {
    WRDE_NOSYS = -1,           /* never: this library has wordexp */
    WRDE_NOSPACE = 1,          /* out of memory */
    WRDE_BADCHAR,              /* an unquoted | & ; < > ( ) { } or newline */
    WRDE_BADVAL,               /* an unset parameter, under WRDE_UNDEF */
    WRDE_CMDSUB,               /* a command substitution, under WRDE_NOCMD */
    WRDE_SYNTAX                /* unbalanced quotes, brackets or the like */
};

int  wordexp(const char *__restrict words, wordexp_t *__restrict we, int flags);
void wordfree(wordexp_t *we);

#ifdef __cplusplus
}
#endif

#endif
