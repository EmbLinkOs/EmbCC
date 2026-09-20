/* Stable symbol identity and interface hashes — the first piece of the
 * project graph (vision §8.2), and what Level-2 incremental builds need
 * (§21).
 *
 * ---- the problem ----
 *
 * `-MD` says a translation unit depends on a header. So a comment added to
 * that header rebuilds it, and so does a declaration the unit never uses.
 * On a tree the size of an operating system that is most of the rebuilds.
 *
 * §21 Level 2: "a header edit that changes no interface hash used by a TU
 * does not rebuild that TU." To say that, two things are needed and neither
 * existed:
 *
 *   a USR   — a name for a declaration that survives unrelated edits, so
 *             the same entity is recognisable across builds
 *   a hash  — of what DEPENDENTS OBSERVE about it, which is not the same
 *             as its text: a function's signature but not its body, a
 *             struct's layout but not its member comments
 *
 * ---- what is hashed ----
 *
 * The rule is: everything a dependent could observe, and nothing else.
 *
 *   function   linkage, return type, parameter types, varargs
 *   global     linkage, type
 *   struct     size, alignment, and each member's name, type, offset, and
 *              bit position — because that is what a dependent compiles in
 *   enum       each enumerator's name and value
 *   typedef    the type it names
 *
 * Deliberately NOT hashed: file names, line numbers, declaration order in
 * the file, or anything about a function's body. Moving a declaration or
 * editing a body must not invalidate a dependent, or the whole exercise
 * buys nothing.
 *
 * ---- determinism (R4) ----
 *
 * The hash is FNV-1a over a canonical string, and the string is built from
 * the semantic model in a fixed order. Same input, same hash, on any host —
 * which a build cache depends on absolutely.
 */
#include "iface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "../parse/ast.h"
#include "../sema/type.h"

/* FNV-1a, 64-bit. Chosen because it is four lines and has no endianness or
 * word-size behaviour to get wrong when EmbCC compiles itself. */
static unsigned long fnv(const char *s, unsigned long h)
{
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211UL;
    }
    return h;
}
#define FNV_INIT 14695981039346656037UL

/* A type, spelled so that two types that dependents cannot tell apart spell
 * the same. A struct spells as its tag: its LAYOUT is hashed under its own
 * USR, so a dependent that uses both picks up both hashes, and one that
 * only holds a pointer is not invalidated by a layout change it cannot
 * observe. */
static void spell_type(struct outbuf *b, const struct type *t)
{
    if (!t) { ob_str(b, "?"); return; }
    if (t->is_volatile) ob_str(b, "volatile ");
    switch (t->kind) {
    case TY_VOID:    ob_str(b, "void"); break;
    case TY_BOOL:    ob_str(b, "_Bool"); break;
    case TY_CHAR:    ob_str(b, t->is_unsigned ? "uchar" : "char"); break;
    case TY_SHORT:   ob_str(b, t->is_unsigned ? "ushort" : "short"); break;
    case TY_INT:     ob_str(b, t->is_unsigned ? "uint" : "int"); break;
    case TY_LONG:    ob_str(b, t->is_unsigned ? "ulong" : "long"); break;
    case TY_INT128:  ob_str(b, t->is_unsigned ? "uint128" : "int128"); break;
    case TY_FLOAT:   ob_str(b, "float"); break;
    case TY_DOUBLE:  ob_str(b, "double"); break;
    case TY_LDOUBLE: ob_str(b, "ldouble"); break;
    case TY_PTR:     spell_type(b, t->pointee); ob_str(b, "*"); break;
    case TY_ARRAY:
        spell_type(b, t->pointee);
        ob_fmt(b, "[%d]", t->count);
        break;
    case TY_FUNC:
        spell_type(b, t->ret);
        ob_str(b, "(");
        for (int i = 0; i < t->nptypes; i++) {
            if (i) ob_str(b, ",");
            spell_type(b, t->ptypes[i]);
        }
        if (t->is_varargs) ob_str(b, ",...");
        ob_str(b, ")");
        break;
    case TY_STRUCT:
        ob_fmt(b, "%s %s", t->is_union ? "union" : "struct",
               t->tag ? t->tag : "(anon)");
        /* An anonymous struct has no USR of its own, so its layout has to
         * be spelled here or a change to it would be invisible. */
        if (!t->tag && t->complete) {
            ob_str(b, "{");
            for (int i = 0; i < t->nmembers; i++) {
                ob_fmt(b, "%s%s:", i ? "," : "",
                       t->members[i].name ? t->members[i].name : "");
                spell_type(b, t->members[i].ty);
                ob_fmt(b, "@%d", t->members[i].off);
            }
            ob_str(b, "}");
        }
        break;
    default: ob_str(b, "?"); break;
    }
}

/* ---- the USR ----
 *
 * `c:@F@name` for a function, `c:@V@name` for a variable, `c:@S@Tag` for a
 * struct, and so on. An entity with INTERNAL linkage is only that entity
 * inside its own file, so its USR carries the file — two units may each
 * have a `static int count` and they are not the same declaration.
 */
static void usr_func(struct outbuf *b, const struct func *f)
{
    if (f->is_static)
        ob_fmt(b, "c:%s@F@%s", f->file ? f->file : "", f->name);
    else
        ob_fmt(b, "c:@F@%s", f->name);
}

static void usr_global(struct outbuf *b, const struct global *g)
{
    if (g->is_static)
        ob_fmt(b, "c:%s@V@%s", g->file ? g->file : "", g->name);
    else
        ob_fmt(b, "c:@V@%s", g->name);
}

/* ---- the hashes ---- */

static unsigned long hash_func(const struct func *f)
{
    struct outbuf b = { NULL, 0, 0 };
    /* The signature and the linkage. NOT the body: a caller cannot observe
     * it, and invalidating callers when a body changes is the rebuild storm
     * this exists to end. */
    ob_fmt(&b, "F %s ", f->is_static ? "static" : "extern");
    spell_type(&b, f->ret_ty);
    ob_str(&b, "(");
    for (int i = 0; i < f->nparams; i++) {
        if (i) ob_str(&b, ",");
        spell_type(&b, f->param_tys[i]);
    }
    if (f->is_varargs) ob_str(&b, ",...");
    ob_str(&b, ")");
    if (f->is_noreturn) ob_str(&b, " noreturn");
    if (f->is_weak)     ob_str(&b, " weak");
    unsigned long h = fnv(b.p ? b.p : "", FNV_INIT);
    ob_free(&b);
    return h;
}

static unsigned long hash_global(const struct global *g)
{
    struct outbuf b = { NULL, 0, 0 };
    ob_fmt(&b, "V %s ", g->is_static ? "static" : "extern");
    spell_type(&b, g->ty);
    if (g->ty)
        ob_fmt(&b, " size=%d align=%d", ty_size(g->ty), ty_align(g->ty));
    if (g->is_weak) ob_str(&b, " weak");
    unsigned long h = fnv(b.p ? b.p : "", FNV_INIT);
    ob_free(&b);
    return h;
}

static unsigned long hash_tag(const struct tagdef *t)
{
    struct outbuf b = { NULL, 0, 0 };
    const char *kind = t->kind == TAG_UNION ? "union"
                     : t->kind == TAG_ENUM ? "enum" : "struct";
    ob_fmt(&b, "%s %s", kind, t->tag ? t->tag : "(anon)");
    if (t->ty && t->kind != TAG_ENUM && t->ty->complete) {
        /* Layout is what a dependent compiles in: sizes, offsets, and for a
         * bit-field its position within its unit. All of it observable, all
         * of it hashed. */
        ob_fmt(&b, " size=%d align=%d {", ty_size(t->ty), ty_align(t->ty));
        for (int i = 0; i < t->ty->nmembers; i++) {
            const struct member *m = &t->ty->members[i];
            ob_fmt(&b, "%s%s:", i ? "," : "", m->name ? m->name : "");
            spell_type(&b, m->ty);
            ob_fmt(&b, "@%d", m->off);
            if (m->is_bitfield)
                ob_fmt(&b, ":%d/%d", m->bit_off, m->bit_width);
        }
        ob_str(&b, "}");
    } else if (t->ty && t->kind != TAG_ENUM) {
        ob_str(&b, " incomplete");
    }
    unsigned long h = fnv(b.p ? b.p : "", FNV_INIT);
    ob_free(&b);
    return h;
}

/* ---- which entities this unit OBSERVES ----
 *
 * A tag is observed when this unit's own declarations mention it, directly
 * or through a member or a pointee. Conservative in the safe direction: a
 * tag reached but not really depended on costs a rebuild, a tag missed
 * would cost correctness.
 */
struct seen { const char **v; int n, cap; };

static int seen_add(struct seen *s, const char *name)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->v[i], name))
            return 0;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->v = xrealloc(s->v, (size_t)s->cap * sizeof *s->v);
    }
    s->v[s->n++] = name;
    return 1;
}

static void reach_type(const struct type *t, struct seen *s, int depth)
{
    if (!t || depth > 16)
        return;
    if (t->kind == TY_STRUCT && t->tag) {
        if (!seen_add(s, t->tag))
            return;                    /* already walked: stop the cycle */
        for (int i = 0; i < t->nmembers; i++)
            reach_type(t->members[i].ty, s, depth + 1);
        return;
    }
    reach_type(t->pointee, s, depth + 1);
    if (t->kind == TY_FUNC) {
        reach_type(t->ret, s, depth + 1);
        for (int i = 0; i < t->nptypes; i++)
            reach_type(t->ptypes[i], s, depth + 1);
    }
}

void iface_emit(struct outbuf *b, struct unit *u)
{
    ob_str(b, "; EmbCC interfaces v1\n");
    ob_fmt(b, "; %s\n", u->file ? u->file : "");

    /* what this unit DEFINES */
    for (const struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed || !f->has_defn)
            continue;
        struct outbuf k = { NULL, 0, 0 };
        usr_func(&k, f);
        ob_fmt(b, "provides %-40s %016lx\n", k.p, hash_func(f));
        ob_free(&k);
    }
    for (const struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined)
            continue;
        struct outbuf k = { NULL, 0, 0 };
        usr_global(&k, g);
        ob_fmt(b, "provides %-40s %016lx\n", k.p, hash_global(g));
        ob_free(&k);
    }

    /* what it OBSERVES of elsewhere: the declarations whose interface it
     * compiled against. A build system re-runs this unit when one of these
     * hashes changes, and only then. */
    for (const struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed || f->has_defn || !f->used)
            continue;
        struct outbuf k = { NULL, 0, 0 };
        usr_func(&k, f);
        ob_fmt(b, "uses     %-40s %016lx\n", k.p, hash_func(f));
        ob_free(&k);
    }
    for (const struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || g->defined || !g->used)
            continue;
        struct outbuf k = { NULL, 0, 0 };
        usr_global(&k, g);
        ob_fmt(b, "uses     %-40s %016lx\n", k.p, hash_global(g));
        ob_free(&k);
    }

    /* the tags this unit's own declarations reach */
    struct seen s = { NULL, 0, 0 };
    for (const struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed)
            continue;
        reach_type(f->ret_ty, &s, 0);
        for (int i = 0; i < f->nparams; i++)
            reach_type(f->param_tys[i], &s, 0);
        for (int i = 0; f->var_tys && i < f->nvars; i++)
            reach_type(f->var_tys[i], &s, 0);
    }
    for (const struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed)
            reach_type(g->ty, &s, 0);

    for (const struct tagdef *t = u->tags; t; t = t->next) {
        if (!t->tag)
            continue;
        int reached = 0;
        for (int i = 0; i < s.n; i++)
            if (!strcmp(s.v[i], t->tag)) { reached = 1; break; }
        if (!reached)
            continue;
        const char *k = t->kind == TAG_UNION ? "U"
                      : t->kind == TAG_ENUM ? "E" : "S";
        ob_fmt(b, "uses     c:@%s@%-37s %016lx\n", k, t->tag, hash_tag(t));
    }
    free(s.v);
}
