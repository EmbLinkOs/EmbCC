/* Scopes and name lookup. A scope is a namespace, a class's member scope, an
 * enum's enumerators, a block, or a function's parameters; lookup walks
 * outward from where the parser is. A namespace is one scope however many
 * times it is reopened, and a using-directive makes another namespace's names
 * visible from it.
 *
 * Symbols live in one hash table keyed by (scope, name): a unit that
 * includes libstdc++ puts thousands of names in `std`, looked up far more
 * often than declared.
 *
 * C's rule survives in C++: a class and a function or variable of the same
 * name may share a scope (`struct stat` and `stat()`), and plain lookup then
 * finds the function — the class only through `struct stat`. */
#include "cxx.h"

#include <string.h>

#include "../driver/util.h"

struct cscope *cx_global;
struct cscope *cx_scope;

#define NBUCKETS 16384
static struct csym *buckets[NBUCKETS];

static unsigned hash(const struct cscope *s, const char *name)
{
    unsigned h = (unsigned)((unsigned long)s >> 4) * 2654435761u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = h * 31 + *p;
    return h & (NBUCKETS - 1);
}

void scope_init(void)
{
    memset(buckets, 0, sizeof buckets);
    cx_scope = NULL;
    cx_global = scope_new(SC_NAMESPACE, NULL, NULL);
    cx_scope = cx_global;
}

struct cscope *scope_new(enum cscope_kind k, const char *name,
                         struct cscope *parent)
{
    struct cscope *s = xcalloc(1, sizeof *s);
    s->k = k;
    s->name = name;
    s->parent = parent;
    if (parent)
        s->fn = parent->fn;
    return s;
}

struct cscope *scope_push(enum cscope_kind k, const char *name)
{
    struct cscope *s = scope_new(k, name, cx_scope);
    cx_scope = s;
    return s;
}

void scope_pop(void)
{
    cx_scope = cx_scope->parent;
}

struct csym *scope_add(struct cscope *s, enum csym_kind k, const char *name)
{
    struct csym *y = xcalloc(1, sizeof *y);
    y->k = k;
    y->name = name;
    y->scope = s;
    y->next = s->syms;
    s->syms = y;
    if (name) {
        unsigned h = hash(s, name);
        y->hnext = buckets[h];
        buckets[h] = y;
    }
    return y;
}

static int is_tag(const struct csym *y)
{
    return y->k == CS_CLASS || y->k == CS_ENUM;
}

/* The symbol `name` in s: the most recent declaration, except that a
 * function or variable hides a class or enum of the same name. */
struct csym *scope_find_here(struct cscope *s, const char *name)
{
    struct csym *tag = NULL;
    for (struct csym *y = buckets[hash(s, name)]; y; y = y->hnext) {
        if (y->scope != s || strcmp(y->name, name) != 0)
            continue;
        if (!is_tag(y))
            return y;
        if (!tag)
            tag = y;
    }
    return tag;
}

struct csym *scope_find_tag(struct cscope *s, const char *name)
{
    for (struct csym *y = buckets[hash(s, name)]; y; y = y->hnext)
        if (y->scope == s && is_tag(y) && strcmp(y->name, name) == 0)
            return y;
    return NULL;
}

/* A name in `s` or in a namespace a using-directive in `s` nominates
 * (transitively; `depth` guards a cycle of directives). */
static struct csym *find_with_usings(struct cscope *s, const char *name,
                                     int tags, int depth)
{
    struct csym *y = tags ? scope_find_tag(s, name)
                          : scope_find_here(s, name);
    if (y || depth > 16)
        return y;
    for (int i = 0; i < s->nusings; i++) {
        y = find_with_usings(s->usings[i], name, tags, depth + 1);
        if (y)
            return y;
    }
    return NULL;
}

struct csym *lookup(struct cscope *from, const char *name)
{
    for (struct cscope *s = from; s; s = s->parent) {
        struct csym *y = find_with_usings(s, name, 0, 0);
        if (y)
            return y;
    }
    return NULL;
}

struct csym *lookup_tag(struct cscope *from, const char *name)
{
    for (struct cscope *s = from; s; s = s->parent) {
        struct csym *y = find_with_usings(s, name, 1, 0);
        if (y)
            return y;
    }
    return NULL;
}

struct csym *lookup_in(struct cscope *in, const char *name)
{
    return find_with_usings(in, name, 0, 0);
}

struct cscope *enclosing_ns(struct cscope *s)
{
    while (s && s->k != SC_NAMESPACE)
        s = s->parent;
    return s;
}

struct cclass *enclosing_class(struct cscope *s)
{
    for (; s; s = s->parent) {
        if (s->k == SC_CLASS)
            return s->cls;
        if (s->k == SC_NAMESPACE)
            return NULL;
    }
    return NULL;
}
