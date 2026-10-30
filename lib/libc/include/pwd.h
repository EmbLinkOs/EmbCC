/* <pwd.h> -- the user database: /etc/passwd (src/posix/pwd.c) */
#ifndef _PWD_H
#define _PWD_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
    char *pw_name, *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos, *pw_dir, *pw_shell;
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t n,
               struct passwd **result);
int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t n,
               struct passwd **result);
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);

#ifdef __cplusplus
}
#endif

#endif
