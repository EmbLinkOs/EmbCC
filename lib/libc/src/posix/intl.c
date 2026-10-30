/* <libintl.h>. The "C" locale's messages are the program's own strings,
 * so every lookup answers with the msgid -- or, for the plural forms,
 * with msgid when n is 1 and the plural otherwise, as glibc does there.
 * What a program sets (its text domain, where a domain's catalogs are,
 * their encoding) is kept and given back. */
#include <libintl.h>
#include <stdlib.h>
#include <string.h>

#include "posix.h"

WEAK char *gettext(const char *msgid) { return (char *)msgid; }

WEAK char *dgettext(const char *domain, const char *msgid)
{
    (void)domain;
    return (char *)msgid;
}

WEAK char *dcgettext(const char *domain, const char *msgid, int category)
{
    (void)domain;
    (void)category;
    return (char *)msgid;
}

WEAK char *ngettext(const char *msgid, const char *plural, unsigned long n)
{
    return (char *)(n == 1 ? msgid : plural);
}

WEAK char *dngettext(const char *domain, const char *msgid, const char *plural,
                     unsigned long n)
{
    (void)domain;
    return ngettext(msgid, plural, n);
}

WEAK char *dcngettext(const char *domain, const char *msgid, const char *plural,
                      unsigned long n, int category)
{
    (void)domain;
    (void)category;
    return ngettext(msgid, plural, n);
}

static char *g_domain;

WEAK char *textdomain(const char *domain)
{
    if (domain) {
        char *d = strdup(*domain ? domain : "messages");
        if (!d)
            return 0;
        free(g_domain);
        g_domain = d;
    }
    return g_domain ? g_domain : (char *)"messages";
}

static struct binding {
    struct binding *next;
    char *domain, *dir, *codeset;
} *g_bind;

static struct binding *bound(const char *domain, int make)
{
    for (struct binding *b = g_bind; b; b = b->next)
        if (!strcmp(b->domain, domain))
            return b;
    if (!make)
        return 0;
    struct binding *b = calloc(1, sizeof *b);
    if (!b || !(b->domain = strdup(domain))) {
        free(b);
        return 0;
    }
    b->next = g_bind;
    g_bind = b;
    return b;
}

static int keep(char **slot, const char *v)
{
    char *c = strdup(v);
    if (!c)
        return -1;
    free(*slot);
    *slot = c;
    return 0;
}

WEAK char *bindtextdomain(const char *domain, const char *dir)
{
    if (!domain || !*domain)
        return 0;
    struct binding *b = bound(domain, dir != 0);
    if (dir && (!b || keep(&b->dir, dir)))
        return 0;
    return b && b->dir ? b->dir : (char *)"/usr/share/locale";
}

WEAK char *bind_textdomain_codeset(const char *domain, const char *codeset)
{
    if (!domain || !*domain)
        return 0;
    struct binding *b = bound(domain, codeset != 0);
    if (codeset && (!b || keep(&b->codeset, codeset)))
        return 0;
    return b ? b->codeset : 0;
}
