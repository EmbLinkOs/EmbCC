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

/* "" asks for the native locale, and here the native locale is "C";
 * C.UTF-8 is "C" too, since this "C" encodes characters as UTF-8.
 * Anything else is a locale this target does not have, and saying so is
 * better than silently giving "C" under another name. */
static int is_c(const char *name)
{
    return name[0] == '\0' || strcmp(name, "C") == 0 ||
           strcmp(name, "POSIX") == 0 || strcmp(name, "C.UTF-8") == 0 ||
           strcmp(name, "C.utf8") == 0 || strcmp(name, "C.utf-8") == 0;
}

char *setlocale(int category, const char *locale)
{
    if (category < LC_ALL || category > LC_IDENTIFICATION)
        return NULL;
    if (!locale)
        return current;           /* a query: report what is in effect */
    return is_c(locale) ? current : NULL;
}

struct lconv *localeconv(void) { return &c_locale; }

/* ---- POSIX's locale objects ------------------------------------------------
 *
 * One locale, so one object. newlocale hands it out for any name
 * setlocale accepts and refuses the rest as POSIX says (ENOENT), and
 * freeing it is nothing to do. */
#include <errno.h>

struct __locale_struct { int unused; };
static struct __locale_struct g_c;
static locale_t g_used = LC_GLOBAL_LOCALE;

__attribute__((weak)) locale_t newlocale(int mask, const char *name, locale_t base)
{
    (void)base;
    if ((mask & ~LC_ALL_MASK) || !name) {
        errno = EINVAL;
        return 0;
    }
    if (!is_c(name)) {
        errno = ENOENT;
        return 0;
    }
    return &g_c;
}

__attribute__((weak)) locale_t duplocale(locale_t loc)
{
    (void)loc;
    return &g_c;
}

__attribute__((weak)) void freelocale(locale_t loc) { (void)loc; }

/* the locale in use, LC_GLOBAL_LOCALE until one is set */
__attribute__((weak)) locale_t uselocale(locale_t loc)
{
    locale_t old = g_used;
    if (loc)
        g_used = loc;
    return old;
}

__attribute__((weak)) struct lconv *localeconv_l(locale_t loc)
{
    (void)loc;
    return &c_locale;
}
