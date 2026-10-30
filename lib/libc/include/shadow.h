/* <shadow.h> -- the shadow password database, /etc/shadow
 * (src/posix/shadow.c). */
#ifndef _SHADOW_H
#define _SHADOW_H

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHADOW "/etc/shadow"

struct spwd {
    char *sp_namp;
    char *sp_pwdp;
    long sp_lstchg;            /* days since 1970 of the last change */
    long sp_min, sp_max, sp_warn, sp_inact, sp_expire;
    unsigned long sp_flag;
};

struct spwd *getspnam(const char *name);
int getspnam_r(const char *name, struct spwd *sp, char *buf, size_t n,
               struct spwd **res);
struct spwd *getspent(void);
int getspent_r(struct spwd *sp, char *buf, size_t n, struct spwd **res);
void setspent(void);
void endspent(void);
struct spwd *fgetspent(FILE *f);
int fgetspent_r(FILE *f, struct spwd *sp, char *buf, size_t n,
                struct spwd **res);
struct spwd *sgetspent(const char *s);
int sgetspent_r(const char *s, struct spwd *sp, char *buf, size_t n,
                struct spwd **res);
int putspent(const struct spwd *sp, FILE *f);
int lckpwdf(void);
int ulckpwdf(void);

#ifdef __cplusplus
}
#endif

#endif
