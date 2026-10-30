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
 * "C" -- "POSIX", and "C.UTF-8", since this "C"'s multibyte encoding is
 * UTF-8; it returns NULL for anything else. That is the same interface
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
#define LC_MESSAGES 6
/* GNU's, which name parts of a locale "C" has nothing for */
#define LC_PAPER          7
#define LC_NAME           8
#define LC_ADDRESS        9
#define LC_TELEPHONE      10
#define LC_MEASUREMENT    11
#define LC_IDENTIFICATION 12

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

/* POSIX's locale objects (src/stdlib/locale.c). There is one locale, so
 * there is one object: newlocale gives it for every name setlocale
 * accepts, and each _l function is its plain counterpart. */
#include <bits/locale_t.h>
#define LC_GLOBAL_LOCALE ((locale_t)-1L)
#define LC_CTYPE_MASK          (1 << LC_CTYPE)
#define LC_NUMERIC_MASK        (1 << LC_NUMERIC)
#define LC_TIME_MASK           (1 << LC_TIME)
#define LC_COLLATE_MASK        (1 << LC_COLLATE)
#define LC_MONETARY_MASK       (1 << LC_MONETARY)
#define LC_MESSAGES_MASK       (1 << LC_MESSAGES)
#define LC_PAPER_MASK          (1 << LC_PAPER)
#define LC_NAME_MASK           (1 << LC_NAME)
#define LC_ADDRESS_MASK        (1 << LC_ADDRESS)
#define LC_TELEPHONE_MASK      (1 << LC_TELEPHONE)
#define LC_MEASUREMENT_MASK    (1 << LC_MEASUREMENT)
#define LC_IDENTIFICATION_MASK (1 << LC_IDENTIFICATION)
#define LC_ALL_MASK            0x1ffe
locale_t newlocale(int mask, const char *name, locale_t base);
locale_t duplocale(locale_t loc);
void     freelocale(locale_t loc);
locale_t uselocale(locale_t loc);
struct lconv *localeconv_l(locale_t loc);

#ifdef __cplusplus
}
#endif

#endif
