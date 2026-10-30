/* <search.h>.
 *
 * tsearch's tree is AVL: POSIX requires nothing of the shape, but a
 * program that inserts keys in sorted order -- the common case, reading
 * a sorted file -- turns an unbalanced tree into a list and every
 * lookup into a scan. A node's first member is the key pointer, which is
 * what POSIX promises of the pointer tsearch returns: *(void **)node is
 * the key.
 *
 * hsearch's table is open addressing over string keys, and grows: POSIX
 * lets ENTER fail once the table is full, and a program sized with
 * hcreate(100) that enters a hundred and first key is better served by
 * a bigger table than by an ENOMEM.
 */
#include <search.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- trees ------------------------------------------------------------ */

struct node {
    const void *key;          /* first: see above */
    struct node *a[2];        /* less, greater */
    int h;                    /* height: a leaf is 1 */
};

typedef int (*cmp_fn)(const void *, const void *);

static int height(const struct node *n) { return n ? n->h : 0; }

static void fix(struct node *n)
{
    int l = height(n->a[0]), r = height(n->a[1]);
    n->h = (l > r ? l : r) + 1;
}

/* n's child on side !dir becomes the root of this subtree */
static struct node *rotate(struct node *n, int dir)
{
    struct node *r = n->a[!dir];
    n->a[!dir] = r->a[dir];
    r->a[dir] = n;
    fix(n);
    fix(r);
    return r;
}

static struct node *rebalance(struct node *n)
{
    fix(n);
    int d = height(n->a[0]) - height(n->a[1]);
    if (d > 1) {
        if (height(n->a[0]->a[0]) < height(n->a[0]->a[1]))
            n->a[0] = rotate(n->a[0], 0);
        return rotate(n, 1);
    }
    if (d < -1) {
        if (height(n->a[1]->a[1]) < height(n->a[1]->a[0]))
            n->a[1] = rotate(n->a[1], 1);
        return rotate(n, 0);
    }
    return n;
}

/* *found is the node with key, new or old; NULL when there was no memory
 * for a new one, and the tree is then unchanged */
static struct node *insert(struct node *n, const void *key, cmp_fn cmp,
                           struct node **found)
{
    if (!n) {
        n = malloc(sizeof *n);
        if (n) {
            n->key = key;
            n->a[0] = n->a[1] = NULL;
            n->h = 1;
        }
        *found = n;
        return n;
    }
    int c = cmp(key, n->key);
    if (c == 0) {
        *found = n;
        return n;
    }
    n->a[c > 0] = insert(n->a[c > 0], key, cmp, found);
    return rebalance(n);
}

__attribute__((weak)) void *tsearch(const void *key, void **root, cmp_fn cmp)
{
    if (!root)
        return NULL;
    struct node *found;
    *root = insert(*root, key, cmp, &found);
    return found;
}

__attribute__((weak)) void *tfind(const void *key, void *const *root, cmp_fn cmp)
{
    if (!root)
        return NULL;
    for (struct node *n = *root; n; ) {
        int c = cmp(key, n->key);
        if (c == 0)
            return n;
        n = n->a[c > 0];
    }
    return NULL;
}

/* the subtree without its least node, which is handed back in *min */
static struct node *pop_min(struct node *n, struct node **min)
{
    if (!n->a[0]) {
        *min = n;
        return n->a[1];
    }
    n->a[0] = pop_min(n->a[0], min);
    return rebalance(n);
}

/* A deleted node with two children is replaced by its successor NODE,
 * moved, rather than by the successor's key copied in: a pointer a
 * caller holds from tsearch must go on naming the key it named. */
static struct node *del(struct node *n, const void *key, cmp_fn cmp,
                        struct node *up, struct node **parent, int *done)
{
    if (!n)
        return NULL;
    int c = cmp(key, n->key);
    if (c) {
        n->a[c > 0] = del(n->a[c > 0], key, cmp, n, parent, done);
        return *done ? rebalance(n) : n;
    }
    *done = 1;
    *parent = up;
    struct node *r;
    if (!n->a[0] || !n->a[1])
        r = n->a[0] ? n->a[0] : n->a[1];
    else {
        struct node *m;
        struct node *right = pop_min(n->a[1], &m);
        m->a[0] = n->a[0];
        m->a[1] = right;
        r = rebalance(m);
    }
    free(n);
    return r;
}

/* The deleted node's parent; for the root, which has none, any pointer
 * that is not null (POSIX's words) -- the root's address here. */
__attribute__((weak)) void *tdelete(const void *restrict key, void **restrict root, cmp_fn cmp)
{
    if (!root || !*root)
        return NULL;
    struct node *parent = NULL;
    int done = 0;
    *root = del(*root, key, cmp, NULL, &parent, &done);
    if (!done)
        return NULL;
    return parent ? (void *)parent : (void *)root;
}

static void walk(const struct node *n, void (*act)(const void *, VISIT, int),
                 int depth)
{
    if (!n->a[0] && !n->a[1]) {
        act(n, leaf, depth);
        return;
    }
    act(n, preorder, depth);
    if (n->a[0])
        walk(n->a[0], act, depth + 1);
    act(n, postorder, depth);
    if (n->a[1])
        walk(n->a[1], act, depth + 1);
    act(n, endorder, depth);
}

__attribute__((weak)) void twalk(const void *root, void (*act)(const void *, VISIT, int))
{
    if (root && act)
        walk(root, act, 0);
}

__attribute__((weak)) void tdestroy(void *root, void (*free_key)(void *))
{
    struct node *n = root;
    if (!n)
        return;
    tdestroy(n->a[0], free_key);
    tdestroy(n->a[1], free_key);
    if (free_key)
        free_key((void *)n->key);
    free(n);
}

/* ---- the hash table ---------------------------------------------------- */

struct __htab {
    ENTRY *e;                 /* key NULL: an empty slot */
    size_t mask, used;
};

/* FNV-1a, in 32 bits whatever size_t is: on AVR it is 16, and the
 * offset basis does not fit */
static size_t hash(const char *s)
{
    unsigned long h = 2166136261ul;
    while (*s)
        h = ((h ^ (unsigned char)*s++) * 16777619ul) & 0xFFFFFFFFul;
    return (size_t)(h ^ h >> 16);
}

static ENTRY *slot(struct __htab *t, const char *key)
{
    for (size_t i = hash(key) & t->mask;; i = (i + 1) & t->mask)
        if (!t->e[i].key || strcmp(t->e[i].key, key) == 0)
            return &t->e[i];
}

static int resize(struct __htab *t, size_t want)
{
    size_t n = 8;
    while (n < want + want / 3 + 1)   /* at most three quarters full */
        n *= 2;
    ENTRY *e = calloc(n, sizeof *e);
    if (!e)
        return -1;
    ENTRY *old = t->e;
    size_t oldn = old ? t->mask + 1 : 0;
    t->e = e;
    t->mask = n - 1;
    for (size_t i = 0; i < oldn; i++)
        if (old[i].key)
            *slot(t, old[i].key) = old[i];
    free(old);
    return 0;
}

__attribute__((weak)) int hcreate_r(size_t nel, struct hsearch_data *h)
{
    struct __htab *t = calloc(1, sizeof *t);
    if (!t || resize(t, nel) < 0) {
        free(t);
        errno = ENOMEM;
        return 0;
    }
    h->__tab = t;
    return 1;
}

__attribute__((weak)) void hdestroy_r(struct hsearch_data *h)
{
    if (h && h->__tab) {
        free(h->__tab->e);
        free(h->__tab);
        h->__tab = NULL;
    }
}

/* 1 with *ret at the entry, or 0 with errno: ESRCH for a FIND that found
 * nothing, ENOMEM for an ENTER there was no room for. */
__attribute__((weak)) int hsearch_r(ENTRY item, ACTION action, ENTRY **ret, struct hsearch_data *h)
{
    struct __htab *t = h->__tab;
    ENTRY *e = slot(t, item.key);
    if (!e->key) {
        if (action == FIND) {
            *ret = NULL;
            errno = ESRCH;
            return 0;
        }
        if ((t->used + 1) * 4 > (t->mask + 1) * 3) {
            if (resize(t, t->used + 1) < 0) {
                *ret = NULL;
                errno = ENOMEM;
                return 0;
            }
            e = slot(t, item.key);
        }
        *e = item;
        t->used++;
    }
    *ret = e;
    return 1;
}

static struct hsearch_data global;

__attribute__((weak)) int hcreate(size_t nel)
{
    if (global.__tab)
        return 0;               /* one table at a time, as POSIX has it */
    return hcreate_r(nel, &global);
}

__attribute__((weak)) void hdestroy(void) { hdestroy_r(&global); }

__attribute__((weak)) ENTRY *hsearch(ENTRY item, ACTION action)
{
    ENTRY *r;
    if (!global.__tab)
        return NULL;
    return hsearch_r(item, action, &r, &global) ? r : NULL;
}

/* ---- linear search, and queues ---------------------------------------- */

__attribute__((weak)) void *lfind(const void *key, const void *base, size_t *nel, size_t width,
            cmp_fn cmp)
{
    const char *p = base;
    for (size_t i = 0; i < *nel; i++, p += width)
        if (cmp(key, p) == 0)
            return (void *)p;
    return NULL;
}

__attribute__((weak)) void *lsearch(const void *key, void *base, size_t *nel, size_t width,
              cmp_fn cmp)
{
    void *p = lfind(key, base, nel, width, cmp);
    if (p)
        return p;
    p = (char *)base + *nel * width;
    memcpy(p, key, width);
    ++*nel;
    return p;
}

/* An element of a queue starts with its forward and backward links. */
struct qelem { struct qelem *next, *prev; };

/* After pred, or as a one-element queue when pred is NULL. */
__attribute__((weak)) void insque(void *elem, void *pred)
{
    struct qelem *e = elem, *p = pred;
    if (!p) {
        e->next = e->prev = NULL;
        return;
    }
    e->next = p->next;
    e->prev = p;
    p->next = e;
    if (e->next)
        e->next->prev = e;
}

__attribute__((weak)) void remque(void *elem)
{
    struct qelem *e = elem;
    if (e->next)
        e->next->prev = e->prev;
    if (e->prev)
        e->prev->next = e->next;
}
