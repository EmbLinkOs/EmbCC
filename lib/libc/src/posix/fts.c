/* <fts.h>, BSD's traversal, in the order BSD's fts reports it: a
 * directory as FTS_D, then what is in it, then the directory again as
 * FTS_DP -- or FTS_DNR in place of FTS_DP when it could not be read.
 * It works on whole paths and never changes directory, so every walk is
 * an FTS_NOCHDIR walk and fts_accpath is fts_path.
 *
 * A directory's entries are read, stat'ed and sorted (by the compar
 * function, if one was given) when the walk enters it, and freed when it
 * leaves; fts_children reads them early and the walk then uses those. */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fts.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "posix.h"

static FTSENT *make(const char *path, size_t plen, const char *name,
                    size_t nlen, FTSENT *parent)
{
    FTSENT *e = calloc(1, sizeof *e + nlen);
    if (!e)
        return 0;
    e->fts_path = malloc(plen + 1);
    if (!e->fts_path) {
        free(e);
        return 0;
    }
    memcpy(e->fts_path, path, plen);
    e->fts_path[plen] = 0;
    e->fts_accpath = e->fts_path;
    e->fts_pathlen = (unsigned short)plen;
    memcpy(e->fts_name, name, nlen);
    e->fts_name[nlen] = 0;
    e->fts_namelen = (unsigned short)nlen;
    e->fts_parent = parent;
    e->fts_level = parent ? (short)(parent->fts_level + 1) : FTS_ROOTLEVEL;
    e->fts_statp = &e->__st;
    e->fts_symfd = -1;
    return e;
}

static void drop(FTSENT *e)
{
    while (e) {
        FTSENT *next = e->fts_link;
        drop(e->__children);
        free(e->fts_path);
        free(e);
        e = next;
    }
}

/* fts_info from a stat -- following symbolic links when asked */
static void classify(FTS *fts, FTSENT *e, int follow)
{
    e->fts_errno = 0;
    if (fts->fts_options & FTS_NOSTAT && e->fts_level > FTS_ROOTLEVEL &&
        e->fts_info != FTS_D) {
        e->fts_info = FTS_NSOK;
        return;
    }
    int r = follow ? stat(e->fts_accpath, &e->__st) : lstat(e->fts_accpath, &e->__st);
    if (r && follow && errno == ENOENT &&
        lstat(e->fts_accpath, &e->__st) == 0) {
        e->fts_info = FTS_SLNONE;  /* a link to nothing */
        return;
    }
    if (r) {
        e->fts_errno = errno;
        memset(&e->__st, 0, sizeof e->__st);
        e->fts_info = FTS_NS;
        return;
    }
    e->fts_dev = e->__st.st_dev;
    e->fts_ino = e->__st.st_ino;
    e->fts_nlink = e->__st.st_nlink;
    if (S_ISDIR(e->__st.st_mode)) {
        e->fts_info = FTS_D;
        if (!strcmp(e->fts_name, ".") || !strcmp(e->fts_name, ".."))
            e->fts_info = e->fts_level > FTS_ROOTLEVEL ? FTS_DOT : FTS_D;
        for (FTSENT *a = e->fts_parent; a && a->fts_level >= FTS_ROOTLEVEL;
             a = a->fts_parent)
            if (a->fts_dev == e->fts_dev && a->fts_ino == e->fts_ino) {
                e->fts_info = FTS_DC;
                e->fts_cycle = a;
                break;
            }
    } else if (S_ISLNK(e->__st.st_mode)) {
        e->fts_info = FTS_SL;
    } else if (S_ISREG(e->__st.st_mode)) {
        e->fts_info = FTS_F;
    } else {
        e->fts_info = FTS_DEFAULT;
    }
}

static int follows(const FTS *fts, int root)
{
    return (fts->fts_options & FTS_LOGICAL) ||
           (root && (fts->fts_options & FTS_COMFOLLOW));
}

/* sort a linked list with the program's comparison */
static FTSENT *sorted(FTS *fts, FTSENT *list, int n)
{
    if (!fts->fts_compar || n < 2)
        return list;
    FTSENT **v = malloc((size_t)n * sizeof *v);
    if (!v)
        return list;
    int k = 0;
    for (FTSENT *e = list; e; e = e->fts_link)
        v[k++] = e;
    qsort(v, (size_t)n, sizeof *v,
          (int (*)(const void *, const void *))fts->fts_compar);
    for (int i = 0; i < n; i++)
        v[i]->fts_link = i + 1 < n ? v[i + 1] : 0;
    FTSENT *head = v[0];
    free(v);
    return head;
}

/* p's entries: NULL with p->fts_info FTS_DNR (and fts_errno) when the
 * directory cannot be read, NULL with errno 0 for an empty one */
static FTSENT *build(FTS *fts, FTSENT *p, int nameonly)
{
    DIR *d = opendir(p->fts_accpath);
    if (!d) {
        p->fts_info = FTS_DNR;
        p->fts_errno = errno;
        return 0;
    }
    FTSENT *head = 0, **tail = &head;
    int n = 0;
    struct dirent *de;
    size_t plen = p->fts_pathlen;
    int slash = plen && p->fts_path[plen - 1] != '/';
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (!(fts->fts_options & FTS_SEEDOT) &&
            (!strcmp(nm, ".") || !strcmp(nm, "..")))
            continue;
        size_t nlen = strlen(nm);
        char *path = malloc(plen + 1 + nlen + 1);
        if (!path)
            break;
        memcpy(path, p->fts_path, plen);
        size_t at = plen;
        if (slash)
            path[at++] = '/';
        memcpy(path + at, nm, nlen + 1);
        FTSENT *e = make(path, at + nlen, nm, nlen, p);
        free(path);
        if (!e)
            break;
        if (nameonly)
            e->fts_info = FTS_NSOK;
        else
            classify(fts, e, follows(fts, 0));
        *tail = e;
        tail = &e->fts_link;
        n++;
    }
    closedir(d);
    errno = 0;
    return sorted(fts, head, n);
}

WEAK FTS *fts_open(char *const *paths, int options,
                   int (*compar)(const FTSENT **, const FTSENT **))
{
    if (options & ~FTS_OPTIONMASK) {
        errno = EINVAL;
        return 0;
    }
    FTS *fts = calloc(1, sizeof *fts);
    if (!fts)
        return 0;
    fts->fts_options = options;
    if (options & FTS_LOGICAL)
        fts->fts_options |= FTS_NOCHDIR;
    fts->fts_compar = compar;
    fts->fts_rfd = -1;
    fts->__rootparent = make("", 0, "", 0, 0);
    if (!fts->__rootparent) {
        free(fts);
        return 0;
    }
    fts->__rootparent->fts_level = FTS_ROOTPARENTLEVEL;
    FTSENT *head = 0, **tail = &head;
    int n = 0;
    for (; *paths; paths++) {
        size_t len = strlen(*paths);
        /* its name is the last component, as BSD's and glibc's give it */
        size_t end = len;
        while (end > 1 && (*paths)[end - 1] == '/')
            end--;
        size_t start = end;
        while (start > 0 && (*paths)[start - 1] != '/')
            start--;
        if (start == end)          /* "/" itself */
            start = 0;
        FTSENT *e = make(*paths, len, *paths + start, end - start,
                         fts->__rootparent);
        if (!e) {
            drop(head);
            free(fts->__rootparent);
            free(fts);
            return 0;
        }
        e->fts_level = FTS_ROOTLEVEL;
        classify(fts, e, follows(fts, 1));
        *tail = e;
        tail = &e->fts_link;
        n++;
    }
    fts->__roots = sorted(fts, head, n);
    fts->fts_dev = fts->__roots ? fts->__roots->fts_dev : 0;
    return fts;
}

WEAK FTSENT *fts_read(FTS *fts)
{
    FTSENT *p = fts->fts_cur;
    if (!fts->__started) {
        fts->__started = 1;
        fts->fts_cur = fts->__roots;
        if (fts->fts_cur && fts->fts_cur->fts_info == FTS_D)
            fts->fts_dev = fts->fts_cur->fts_dev;
        return fts->fts_cur;
    }
    if (!p)
        return 0;

    int instr = p->fts_instr;
    p->fts_instr = FTS_NOINSTR;
    if (instr == FTS_AGAIN) {
        classify(fts, p, follows(fts, p->fts_level == FTS_ROOTLEVEL));
        return p;
    }
    if (instr == FTS_FOLLOW && (p->fts_info == FTS_SL || p->fts_info == FTS_SLNONE)) {
        classify(fts, p, 1);
        if (p->fts_info == FTS_D)
            return p;
    }

    /* into a directory just reported as FTS_D */
    if (p->fts_info == FTS_D) {
        int skip = instr == FTS_SKIP ||
                   ((fts->fts_options & FTS_XDEV) && p->fts_dev != fts->fts_dev);
        if (!skip) {
            if (!p->__children)
                p->__children = build(fts, p, 0);
            if (p->__children) {
                fts->fts_cur = p->__children;
                return fts->fts_cur;
            }
            if (p->fts_info == FTS_DNR)
                return p;          /* in place of FTS_DP */
        }
        p->fts_info = FTS_DP;
        return p;
    }

    /* on to the next sibling, or back up to the parent */
    for (;;) {
        if (p->fts_link) {
            fts->fts_cur = p->fts_link;
            if (fts->fts_cur->fts_level == FTS_ROOTLEVEL &&
                fts->fts_cur->fts_info == FTS_D)
                fts->fts_dev = fts->fts_cur->fts_dev;
            return fts->fts_cur;
        }
        FTSENT *parent = p->fts_parent;
        if (!parent || parent->fts_level == FTS_ROOTPARENTLEVEL) {
            fts->fts_cur = 0;
            errno = 0;
            return 0;
        }
        drop(parent->__children);
        parent->__children = 0;
        fts->fts_cur = parent;
        parent->fts_info = FTS_DP;
        return parent;
    }
}

/* the entries of the directory fts_read last returned (the roots before
 * the first fts_read), which the walk will then go through */
WEAK FTSENT *fts_children(FTS *fts, int options)
{
    if (options && options != FTS_NAMEONLY) {
        errno = EINVAL;
        return 0;
    }
    if (!fts->__started)
        return fts->__roots;
    FTSENT *p = fts->fts_cur;
    errno = 0;
    if (!p || p->fts_info != FTS_D)
        return 0;
    if (!p->__children)
        p->__children = build(fts, p, options == FTS_NAMEONLY);
    return p->__children;
}

WEAK int fts_set(FTS *fts, FTSENT *e, int instr)
{
    (void)fts;
    if (instr != 0 && instr != FTS_AGAIN && instr != FTS_FOLLOW &&
        instr != FTS_NOINSTR && instr != FTS_SKIP) {
        errno = EINVAL;
        return -1;
    }
    e->fts_instr = (unsigned short)instr;
    return 0;
}

WEAK int fts_close(FTS *fts)
{
    if (!fts)
        return 0;
    drop(fts->__roots);
    free(fts->__rootparent);
    free(fts);
    return 0;
}
