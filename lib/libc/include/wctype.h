/* <wctype.h> — C11 §7.30.
 *
 * The <ctype.h> questions, asked of a wide character. In the "C" locale
 * the answer for every value below 128 is the narrow one, and above it
 * the answer is "no" -- which is not a shortcut but what the "C" locale
 * MEANS: it classifies exactly the basic execution character set and
 * nothing else. A library that answered `iswalpha(U'é')` yes here would
 * be claiming a locale it does not have.
 *
 * The one place that would matter is a program doing real Unicode work,
 * and such a program needs property tables far beyond what a C locale
 * ever provides -- it wants ICU, not this.
 */
#ifndef _WCTYPE_H
#define _WCTYPE_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

int iswalnum(wint_t c);
int iswalpha(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);

wint_t towlower(wint_t c);
wint_t towupper(wint_t c);

/* The extensible forms: a class or a mapping looked up by NAME, so a
 * program can ask about a category the header does not have a function
 * for. Here the set of names is exactly the twelve above. */
wctype_t wctype(const char *name);
int iswctype(wint_t c, wctype_t type);
wctrans_t wctrans(const char *name);
wint_t towctrans(wint_t c, wctrans_t trans);

#ifdef __cplusplus
}
#endif

#endif
