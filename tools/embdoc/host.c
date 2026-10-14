/* host -- the three things embdoc needs that ISO C cannot do: tell a file
 * from a directory, list a directory, and make one. Everything else in
 * embdoc is ISO C (EmbCC compiles it with its own headers); this is the
 * file a port to another host replaces. POSIX here. */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "embdoc.h"

int host_kind(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 0;
}

char **host_list(const char *dir, int *n)
{
    DIR *d = opendir(dir);
    char **names = NULL;
    int cap = 0;
    *n = 0;
    if (!d)
        return NULL;
    struct dirent *e;
    for (e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 32;
            names = xrealloc(names, (size_t)cap * sizeof *names);
        }
        names[(*n)++] = xstrdup(e->d_name);
    }
    closedir(d);
    return names;
}

int host_mkdir(const char *path)
{
    return mkdir(path, 0777) == 0 || host_kind(path) == 2 ? 0 : -1;
}
