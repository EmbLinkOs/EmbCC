/* <utmpx.h> -- who is logged in (src/posix/utmpx.c): /var/run/utmp in
 * Linux's record layout, 384 bytes each, so the records login and who
 * write there are the ones read here. */
#ifndef _UTMPX_H
#define _UTMPX_H

#include <features.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMPTY         0
#define RUN_LVL       1
#define BOOT_TIME     2
#define NEW_TIME      3
#define OLD_TIME      4
#define INIT_PROCESS  5
#define LOGIN_PROCESS 6
#define USER_PROCESS  7
#define DEAD_PROCESS  8
#define ACCOUNTING    9

#define __UT_LINESIZE 32
#define __UT_NAMESIZE 32
#define __UT_HOSTSIZE 256

struct __exit_status { short e_termination, e_exit; };

struct utmpx {
    short ut_type;
    pid_t ut_pid;
    char ut_line[__UT_LINESIZE];
    char ut_id[4];
    char ut_user[__UT_NAMESIZE];
    char ut_host[__UT_HOSTSIZE];
    struct __exit_status ut_exit;
    int32_t ut_session;
    struct { int32_t tv_sec, tv_usec; } ut_tv;
    int32_t ut_addr_v6[4];
    char __unused[20];
};

void          setutxent(void);
struct utmpx *getutxent(void);
struct utmpx *getutxid(const struct utmpx *ut);
struct utmpx *getutxline(const struct utmpx *ut);
struct utmpx *pututxline(const struct utmpx *ut);
void          endutxent(void);

#if defined(__EMB_GNU)
#define UTMPX_FILE "/var/run/utmp"
#define WTMPX_FILE "/var/log/wtmp"
int  utmpxname(const char *file);
void updwtmpx(const char *file, const struct utmpx *ut);
#endif

#ifdef __cplusplus
}
#endif

#endif
