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

/* Argument-dependent lookup is on: hidden friends are found. */
static int see_hidden;

/* functions all of which only a friend declaration declared */
static int only_hidden_friends(const struct csym *y)
{
    if (y->k != CS_FUNC || !y->fns)
        return 0;
    for (struct cfunc *f = y->fns; f; f = f->next)
        if (!f->hidden_friend)
            return 0;
    return 1;
}

/* Functions of two sets found for one name as one overload set: entries
 * standing for them all (each set is its own list), each once. */
static struct csym *merge_sets(struct csym *y, struct csym *z)
{
    int news = 0;
    for (struct cfunc *g = z->fns; g && !news; g = g->next) {
        struct cfunc *og = g->alias_of ? g->alias_of : g;
        news = 1;
        for (struct cfunc *f = y->fns; f && news; f = f->next)
            if (f == og || f->alias_of == og)
                news = 0;
    }
    if (!news)
        return y;
    struct csym *m = xmalloc(sizeof *m);
    *m = *y;
    m->hnext = m->next = NULL;
    m->fns = NULL;
    struct cfunc **tail = &m->fns;
    struct csym *both[2] = { y, z };
    for (int k = 0; k < 2; k++)
        for (struct cfunc *g = both[k]->fns; g; g = g->next) {
            struct cfunc *og = g->alias_of ? g->alias_of : g;
            int have = 0;
            for (struct cfunc *f = m->fns; f && !have; f = f->next)
                have = f->alias_of == og;
            if (have)
                continue;
            struct cfunc *a = xmalloc(sizeof *a);
            *a = *og;
            a->alias_of = og;
            a->next = NULL;
            *tail = a;
            tail = &a->next;
        }
    return m;
}

/* A name in `s` or in a namespace a using-directive in `s` nominates
 * (transitively; `depth` guards a cycle of directives) — an inline
 * namespace's among them. Functions found in more than one are one
 * overload set (std::rotate: pstl's in std, the algorithm's in std::_V2). */
static struct csym *find_with_usings(struct cscope *s, const char *name,
                                     int tags, int depth)
{
    struct csym *y = tags ? scope_find_tag(s, name)
                          : scope_find_here(s, name);
    if (y && !see_hidden && only_hidden_friends(y))
        y = NULL;                 /* (a hidden friend: not for this) */
    if (depth > 16 || (y && (tags || y->k != CS_FUNC)))
        return y;
    for (int i = 0; i < s->nusings; i++) {
        struct csym *z = find_with_usings(s->usings[i], name, tags,
                                          depth + 1);
        if (!z)
            continue;
        if (!y) {
            y = z;
            if (tags || y->k != CS_FUNC)
                return y;
        } else if (z->k == CS_FUNC) {
            y = merge_sets(y, z);
        }
    }
    return y;
}

/* A class's member `name`: its own, else what its bases have (6.5.2) —
 * the same member found through two bases is one result; two different
 * ones are ambiguous, reported where the name is used. */
int class_lookup_ambiguous;

struct csym *class_member(struct cclass *c, const char *name)
{
    class_ensure(c);
    struct csym *y = scope_find_here(c->scope, name);
    if (y)
        return y;
    struct csym *found = NULL;
    for (int i = 0; i < c->nbases; i++) {
        struct csym *z = class_member(c->bases[i].cls, name);
        if (!z || z == found)
            continue;
        if (found && !(z->k == CS_CLASS && found->k == CS_CLASS &&
                       z->type->cls == found->type->cls))
            class_lookup_ambiguous = 1;
        if (!found)
            found = z;
    }
    return found;
}

static struct csym *find_in(struct cscope *s, const char *name)
{
    if (s->k == SC_CLASS)
        return class_member(s->cls, name);
    return find_with_usings(s, name, 0, 0);
}

struct csym *lookup(struct cscope *from, const char *name)
{
    for (struct cscope *s = from; s; s = s->parent) {
        struct csym *y = find_in(s, name);
        if (y)
            return y->k == CS_PACK ? pack_current(y) : y;
    }
    return NULL;
}

/* A name as declared: a pack stays a pack (sizeof..., expansions). */
struct csym *lookup_raw(struct cscope *from, const char *name)
{
    for (struct cscope *s = from; s; s = s->parent) {
        struct csym *y = find_in(s, name);
        if (y)
            return y;
    }
    return NULL;
}

/* The class or enum `name` a qualified name s::name declares or defines:
 * s's own, or its inline namespaces' (9.8.2.2) */
struct csym *scope_find_tag_inline(struct cscope *s, const char *name)
{
    struct csym *y = scope_find_tag(s, name);
    for (int i = 0; !y && i < s->nusings; i++)
        if (s->usings[i]->is_inline && s->usings[i]->parent == s)
            y = scope_find_tag_inline(s->usings[i], name);
    return y;
}

/* The inline namespace of s (nested, perhaps) declaring `name`, or NULL */
struct cscope *inline_ns_declaring(struct cscope *s, const char *name)
{
    for (int i = 0; i < s->nusings; i++) {
        struct cscope *u = s->usings[i];
        if (!u->is_inline || u->parent != s)
            continue;
        if (scope_find_here(u, name))
            return u;
        struct cscope *in = inline_ns_declaring(u, name);
        if (in)
            return in;
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
    return find_in(in, name);
}

struct csym *lookup_in_adl(struct cscope *in, const char *name)
{
    int saved = see_hidden;
    see_hidden = 1;
    struct csym *y = find_in(in, name);
    see_hidden = saved;
    return y;
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
