/* <regex.h> -- POSIX regular expressions, basic and extended, with
 * glibc's numbers and GNU's common extensions: \+ \? \| in basic syntax,
 * \< \> \b \B \w \W \s \S \` \', back-references in both syntaxes, and
 * REG_STARTEND. Matching is POSIX's: the leftmost match, and of those
 * the longest. */
#ifndef _REGEX_H
#define _REGEX_H

#include <features.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int regoff_t;

typedef struct {
    size_t re_nsub;            /* how many parenthesized subexpressions */
    void *__prog;
    int __cflags;
} regex_t;

typedef struct {
    regoff_t rm_so;            /* where the match starts, -1 for none */
    regoff_t rm_eo;            /* one past where it ends */
} regmatch_t;

#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NEWLINE  4
#define REG_NOSUB    8

#define REG_NOTBOL   1
#define REG_NOTEOL   2
#define REG_STARTEND 4

#define REG_NOERROR  0
#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_ECOLLATE 3
#define REG_ECTYPE   4
#define REG_EESCAPE  5
#define REG_ESUBREG  6
#define REG_EBRACK   7
#define REG_EPAREN   8
#define REG_EBRACE   9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13
#define REG_EEND     14
#define REG_ESIZE    15
#define REG_ERPAREN  16

int    regcomp(regex_t *__restrict re, const char *__restrict pattern, int cflags);
int    regexec(const regex_t *__restrict re, const char *__restrict s,
               size_t nmatch, regmatch_t pmatch[__restrict], int eflags);
size_t regerror(int err, const regex_t *__restrict re, char *__restrict buf,
                size_t n);
void   regfree(regex_t *re);

#ifdef __cplusplus
}
#endif

#endif
