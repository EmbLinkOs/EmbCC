/* <locale.h> — C11 §7.11.
 *
 * One locale, named "C", and the honest reason: a locale is a table of
 * cultural conventions that has to come from somewhere, and on a
 * freestanding target there is nowhere. Pretending otherwise would mean
 * shipping a copy of somebody's idea of a decimal separator and calling
 * it the system's.
 *
 * So `setlocale` accepts "C" and "" -- which the standard says is the
 * implementation-defined native locale, and here the native locale IS
 * "C" -- and returns NULL for anything else. That is the same interface
 * a hosted library offers; a program that checks the result learns the
 * truth, and one that does not is running in "C" either way, which is
 * what it would have got.
 *
 * `localeconv` returns the "C" locale's conventions in full, because
 * the standard specifies exactly what they are and printf's `'` flag
 * and the numeric parsers are entitled to read them.
 */
#ifndef _LOCALE_H
#define _LOCALE_H

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

struct lconv {
    /* Numeric, and the only two the "C" locale gives content to. */
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    /* Monetary: all empty in "C", and CHAR_MAX means "not available",
     * which is the standard's way of saying it and is why these are
     * char rather than int. */
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

#ifdef __cplusplus
}
#endif

#endif
