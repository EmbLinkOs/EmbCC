/* <mntent.h> -- the mounted file systems' table, /proc/mounts (or
 * /etc/mtab, /etc/fstab), read and written a line at a time
 * (src/posix/misc2.c). */
#ifndef _MNTENT_H
#define _MNTENT_H

#include <stdio.h>
#include <paths.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOUNTED    "/etc/mtab"
#define MNTTAB     "/etc/fstab"
#define MNTTYPE_IGNORE "ignore"
#define MNTTYPE_NFS    "nfs"
#define MNTTYPE_SWAP   "swap"
#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RO      "ro"
#define MNTOPT_RW      "rw"
#define MNTOPT_SUID    "suid"
#define MNTOPT_NOSUID  "nosuid"
#define MNTOPT_NOAUTO  "noauto"

struct mntent {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
    char *mnt_opts;
    int mnt_freq;
    int mnt_passno;
};

FILE *setmntent(const char *file, const char *mode);
struct mntent *getmntent(FILE *f);
struct mntent *getmntent_r(FILE *__restrict f, struct mntent *__restrict m,
                           char *__restrict buf, int n);
int addmntent(FILE *__restrict f, const struct mntent *__restrict m);
int endmntent(FILE *f);
char *hasmntopt(const struct mntent *m, const char *opt);

#ifdef __cplusplus
}
#endif

#endif
