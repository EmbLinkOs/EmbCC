/* <grp.h> -- the group database: /etc/group (src/posix/pwd.c) */
#ifndef _GRP_H
#define _GRP_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct group {
    char *gr_name, *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
int getgrnam_r(const char *name, struct group *g, char *buf, size_t n,
               struct group **result);
int getgrgid_r(gid_t gid, struct group *g, char *buf, size_t n,
               struct group **result);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *n);
int initgroups(const char *user, gid_t group);
int setgroups(size_t n, const gid_t *groups);

#ifdef __cplusplus
}
#endif

#endif
