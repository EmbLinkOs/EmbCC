/* <sys/utsname.h> -- the running system's names */
#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65];
    char domainname[65];
};

int uname(struct utsname *u);

#ifdef __cplusplus
}
#endif

#endif
