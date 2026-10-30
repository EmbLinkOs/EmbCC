/* <libintl.h> -- GNU gettext's message catalogs (src/posix/intl.c).
 * There is one locale, "C", whose messages are the program's own: every
 * lookup returns the msgid it was given, as glibc does in "C". The
 * domain and directory bookkeeping is kept, so a program reads back
 * what it set. */
#ifndef _LIBINTL_H
#define _LIBINTL_H

#ifdef __cplusplus
extern "C" {
#endif

char *gettext(const char *msgid);
char *dgettext(const char *domain, const char *msgid);
char *dcgettext(const char *domain, const char *msgid, int category);
char *ngettext(const char *msgid, const char *plural, unsigned long n);
char *dngettext(const char *domain, const char *msgid, const char *plural,
                unsigned long n);
char *dcngettext(const char *domain, const char *msgid, const char *plural,
                 unsigned long n, int category);
char *textdomain(const char *domain);
char *bindtextdomain(const char *domain, const char *dir);
char *bind_textdomain_codeset(const char *domain, const char *codeset);

#ifdef __cplusplus
}
#endif

#endif
