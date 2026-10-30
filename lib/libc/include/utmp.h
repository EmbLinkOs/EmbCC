/* <utmp.h> -- the older names for <utmpx.h>'s database; a struct utmp
 * is a struct utmpx (src/posix/utmpx.c). */
#ifndef _UTMP_H
#define _UTMP_H

#include <utmpx.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UT_LINESIZE __UT_LINESIZE
#define UT_NAMESIZE __UT_NAMESIZE
#define UT_HOSTSIZE __UT_HOSTSIZE
#define _PATH_UTMP  "/var/run/utmp"
#define _PATH_WTMP  "/var/log/wtmp"
#define UTMP_FILE   _PATH_UTMP
#define WTMP_FILE   _PATH_WTMP
#define ut_name     ut_user
#define ut_time     ut_tv.tv_sec
#define ut_xtime    ut_tv.tv_sec
#define ut_addr     ut_addr_v6[0]
#define exit_status __exit_status

/* one record type under two names, as musl has it */
#define utmp utmpx

void         setutent(void);
struct utmp *getutent(void);
struct utmp *getutid(const struct utmp *ut);
struct utmp *getutline(const struct utmp *ut);
struct utmp *pututline(const struct utmp *ut);
void         endutent(void);
int          getutent_r(struct utmp *buf, struct utmp **res);
int          getutid_r(const struct utmp *ut, struct utmp *buf, struct utmp **res);
int          getutline_r(const struct utmp *ut, struct utmp *buf, struct utmp **res);
int          utmpname(const char *file);
void         updwtmp(const char *file, const struct utmp *ut);
void         logwtmp(const char *line, const char *name, const char *host);
void         login(const struct utmp *ut);
int          logout(const char *line);

#ifdef __cplusplus
}
#endif

#endif
