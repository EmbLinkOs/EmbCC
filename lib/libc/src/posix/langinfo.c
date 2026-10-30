/* <langinfo.h> and <nl_types.h>: the "C" locale's answers, which are
 * strftime's and localeconv's, and no message catalogs. */
#include <langinfo.h>
#include <nl_types.h>
#include <errno.h>

#include "posix.h"

static const char *const g_items[__NL_ITEMS] = {
    [CODESET] = "UTF-8",
    [D_T_FMT] = "%a %b %e %H:%M:%S %Y", [D_FMT] = "%m/%d/%y",
    [T_FMT] = "%H:%M:%S", [T_FMT_AMPM] = "%I:%M:%S %p",
    [AM_STR] = "AM", [PM_STR] = "PM",
    [DAY_1] = "Sunday", [DAY_2] = "Monday", [DAY_3] = "Tuesday",
    [DAY_4] = "Wednesday", [DAY_5] = "Thursday", [DAY_6] = "Friday",
    [DAY_7] = "Saturday",
    [ABDAY_1] = "Sun", [ABDAY_2] = "Mon", [ABDAY_3] = "Tue", [ABDAY_4] = "Wed",
    [ABDAY_5] = "Thu", [ABDAY_6] = "Fri", [ABDAY_7] = "Sat",
    [MON_1] = "January", [MON_2] = "February", [MON_3] = "March",
    [MON_4] = "April", [MON_5] = "May", [MON_6] = "June", [MON_7] = "July",
    [MON_8] = "August", [MON_9] = "September", [MON_10] = "October",
    [MON_11] = "November", [MON_12] = "December",
    [ABMON_1] = "Jan", [ABMON_2] = "Feb", [ABMON_3] = "Mar", [ABMON_4] = "Apr",
    [ABMON_5] = "May", [ABMON_6] = "Jun", [ABMON_7] = "Jul", [ABMON_8] = "Aug",
    [ABMON_9] = "Sep", [ABMON_10] = "Oct", [ABMON_11] = "Nov",
    [ABMON_12] = "Dec",
    [ERA] = "", [ERA_D_FMT] = "", [ALT_DIGITS] = "", [ERA_D_T_FMT] = "",
    [ERA_T_FMT] = "",
    [RADIXCHAR] = ".", [THOUSEP] = "", [YESEXPR] = "^[yY]",
    [NOEXPR] = "^[nN]", [CRNCYSTR] = "-",
};

/* an item nobody knows is "", as POSIX asks */
WEAK char *nl_langinfo(nl_item item)
{
    if (item < 0 || item >= __NL_ITEMS || !g_items[item])
        return (char *)"";
    return (char *)g_items[item];
}

WEAK nl_catd catopen(const char *name, int flag)
{
    (void)name;
    (void)flag;
    errno = ENOENT;
    return (nl_catd)-1;
}

WEAK char *catgets(nl_catd cat, int set, int number, const char *message)
{
    (void)cat;
    (void)set;
    (void)number;
    errno = EBADF;
    return (char *)message;
}

WEAK int catclose(nl_catd cat)
{
    (void)cat;
    errno = EBADF;
    return -1;
}
