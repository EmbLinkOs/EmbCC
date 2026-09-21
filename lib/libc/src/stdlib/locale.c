/* setlocale, localeconv — C11 §7.11.
 *
 * The "C" locale, exactly as the standard specifies it, and nothing
 * else. See <locale.h> for why that is the honest answer here rather
 * than a limitation to apologise for.
 */
#include <locale.h>
#include <limits.h>
#include <string.h>

/* C11 §7.11.2.1p3 gives every one of these values for the "C" locale.
 * CHAR_MAX in a monetary field means "not available" -- not "zero" --
 * and a program that formats money must check for it. */
static struct lconv c_locale = {
    (char *)".",              /* decimal_point */
    (char *)"",               /* thousands_sep */
    (char *)"",               /* grouping */
    (char *)"",               /* int_curr_symbol */
    (char *)"",               /* currency_symbol */
    (char *)"",               /* mon_decimal_point */
    (char *)"",               /* mon_thousands_sep */
    (char *)"",               /* mon_grouping */
    (char *)"",               /* positive_sign */
    (char *)"",               /* negative_sign */
    CHAR_MAX, CHAR_MAX,       /* int_frac_digits, frac_digits */
    CHAR_MAX, CHAR_MAX,       /* p_cs_precedes, p_sep_by_space */
    CHAR_MAX, CHAR_MAX,       /* n_cs_precedes, n_sep_by_space */
    CHAR_MAX, CHAR_MAX,       /* p_sign_posn, n_sign_posn */
    CHAR_MAX, CHAR_MAX,       /* int_p_cs_precedes, int_p_sep_by_space */
    CHAR_MAX, CHAR_MAX,       /* int_n_cs_precedes, int_n_sep_by_space */
    CHAR_MAX, CHAR_MAX        /* int_p_sign_posn, int_n_sign_posn */
};

static char current[] = "C";

char *setlocale(int category, const char *locale)
{
    if (category < LC_ALL || category > LC_TIME)
        return NULL;
    if (!locale)
        return current;           /* a query: report what is in effect */
    /* "" asks for the native locale, and here the native locale is "C".
     * Anything else is a locale this target does not have, and saying
     * so is better than silently giving "C" under another name. */
    if (locale[0] == '\0' || strcmp(locale, "C") == 0 ||
        strcmp(locale, "POSIX") == 0)
        return current;
    return NULL;
}

struct lconv *localeconv(void) { return &c_locale; }
