/* <fts.h> -- BSD's file hierarchy traversal (src/posix/fts.c). */
#ifndef _FTS_H
#define _FTS_H

#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ftsent {
    struct _ftsent *fts_cycle;   /* the ancestor a cycle returns to */
    struct _ftsent *fts_parent;
    struct _ftsent *fts_link;    /* the next sibling */
    long fts_number;             /* for the program */
    void *fts_pointer;           /* for the program */
    char *fts_accpath;           /* the path to open it by */
    char *fts_path;              /* the root-relative path */
    int fts_errno;
    int fts_symfd;
    unsigned short fts_pathlen;
    unsigned short fts_namelen;
    ino_t fts_ino;
    dev_t fts_dev;
    nlink_t fts_nlink;
    short fts_level;
    unsigned short fts_info;
    unsigned short fts_flags;
    unsigned short fts_instr;
    struct stat *fts_statp;
    struct _ftsent *__children;  /* the library's */
    struct stat __st;
    char fts_name[1];
} FTSENT;

typedef struct {
    FTSENT *fts_cur;
    FTSENT *fts_child;
    FTSENT **fts_array;
    dev_t fts_dev;
    char *fts_path;
    int fts_rfd;
    int fts_pathlen;
    int fts_nitems;
    int (*fts_compar)(const FTSENT **, const FTSENT **);
    int fts_options;
    FTSENT *__roots;             /* the library's */
    FTSENT *__rootparent;
    int __started;
} FTS;

#define FTS_COMFOLLOW 0x0001
#define FTS_LOGICAL   0x0002
#define FTS_NOCHDIR   0x0004
#define FTS_NOSTAT    0x0008
#define FTS_PHYSICAL  0x0010
#define FTS_SEEDOT    0x0020
#define FTS_XDEV      0x0040
#define FTS_WHITEOUT  0x0080
#define FTS_OPTIONMASK 0x00ff
#define FTS_NAMEONLY  0x0100
#define FTS_STOP      0x0200

#define FTS_ROOTPARENTLEVEL -1
#define FTS_ROOTLEVEL 0

#define FTS_D       1
#define FTS_DC      2
#define FTS_DEFAULT 3
#define FTS_DNR     4
#define FTS_DOT     5
#define FTS_DP      6
#define FTS_ERR     7
#define FTS_F       8
#define FTS_INIT    9
#define FTS_NS     10
#define FTS_NSOK   11
#define FTS_SL     12
#define FTS_SLNONE 13
#define FTS_W      14

#define FTS_DONTCHDIR 0x01
#define FTS_SYMFOLLOW 0x02

#define FTS_AGAIN   1
#define FTS_FOLLOW  2
#define FTS_NOINSTR 3
#define FTS_SKIP    4

FTS    *fts_open(char *const *paths, int options,
                 int (*compar)(const FTSENT **, const FTSENT **));
FTSENT *fts_read(FTS *fts);
FTSENT *fts_children(FTS *fts, int options);
int     fts_set(FTS *fts, FTSENT *e, int instr);
int     fts_close(FTS *fts);

#ifdef __cplusplus
}
#endif

#endif
