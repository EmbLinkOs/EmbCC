/* <dlfcn.h> -- loading code at run time (src/posix/dl.c). This library
 * links programs statically: dlopen(NULL) is the program itself, and a
 * file cannot be opened, which dlerror says in words. */
#ifndef _DLFCN_H
#define _DLFCN_H

#include <features.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY     0x00001
#define RTLD_NOW      0x00002
#define RTLD_NOLOAD   0x00004
#define RTLD_DEEPBIND 0x00008
#define RTLD_GLOBAL   0x00100
#define RTLD_LOCAL    0
#define RTLD_NODELETE 0x01000
#define RTLD_NEXT     ((void *)-1L)
#define RTLD_DEFAULT  ((void *)0)

void *dlopen(const char *file, int mode);
int   dlclose(void *handle);
void *dlsym(void *__restrict handle, const char *__restrict name);
char *dlerror(void);

#if defined(__EMB_GNU)
typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;
int   dladdr(const void *addr, Dl_info *info);
void *dlvsym(void *__restrict handle, const char *__restrict name,
             const char *__restrict version);
int   dlinfo(void *__restrict handle, int request, void *__restrict arg);
#endif

#ifdef __cplusplus
}
#endif

#endif
