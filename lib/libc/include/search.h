/* <search.h> -- POSIX's search tables: binary trees (tsearch), one hash
 * table (hsearch, and GNU's reentrant _r forms), linear search and
 * doubly linked queues (src/posix/search.c). */
#ifndef _SEARCH_H
#define _SEARCH_H

#include <stddef.h>
#include <features.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FIND, ENTER } ACTION;
typedef struct entry { char *key; void *data; } ENTRY;

int    hcreate(size_t nel);
void   hdestroy(void);
ENTRY *hsearch(ENTRY item, ACTION action);

#if defined(__EMB_GNU)
struct hsearch_data {
    struct __htab *__tab;
    size_t __unused1, __unused2;
};
int  hcreate_r(size_t nel, struct hsearch_data *htab);
void hdestroy_r(struct hsearch_data *htab);
int  hsearch_r(ENTRY item, ACTION action, ENTRY **ret,
               struct hsearch_data *htab);
#endif

/* twalk's visits: before a node's children, between them, after them,
 * and the one visit of a node with none */
typedef enum { preorder, postorder, endorder, leaf } VISIT;

void *tsearch(const void *key, void **root,
              int (*cmp)(const void *, const void *));
void *tfind(const void *key, void *const *root,
            int (*cmp)(const void *, const void *));
void *tdelete(const void *__restrict key, void **__restrict root,
              int (*cmp)(const void *, const void *));
void  twalk(const void *root, void (*action)(const void *, VISIT, int));
void  twalk_r(const void *root, void (*action)(const void *, VISIT, void *),
              void *closure);
#if defined(__EMB_GNU)
void  tdestroy(void *root, void (*free_key)(void *));
#endif

void *lsearch(const void *key, void *base, size_t *nel, size_t width,
              int (*cmp)(const void *, const void *));
void *lfind(const void *key, const void *base, size_t *nel, size_t width,
            int (*cmp)(const void *, const void *));

void insque(void *elem, void *pred);
void remque(void *elem);

#ifdef __cplusplus
}
#endif

#endif
