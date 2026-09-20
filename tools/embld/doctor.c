/* `embld --doctor` — why the link failed, in terms a person can act on
 * (docs/tools/diagnostics.md T6).
 *
 * A linker says "undefined reference to `foo'" and stops at the first one.
 * That names the symptom. What the programmer needs is the cause, and the
 * inputs hold it: whether some unit does define the name but privately
 * (`static`, so no other unit can see it), whether it is a C library name
 * and which header declares it, whether it is a C++ vtable — which is
 * undefined for one specific reason worth knowing — or whether nothing on
 * the command line defines it at all, which usually means a source file is
 * missing from the link.
 *
 * Every undefined symbol is reported, not the first, and each with what
 * the inputs say about it.
 */
#include "doctor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/driver/util.h"
#include "../../src/elf/elf.h"

/* ---- what the inputs hold ---- */

struct sym_ent {
    char *name;
    char *where;              /* the object (or archive member) it is in */
    int defined;              /* it has a definition here */
    int local;                /* `static`: no other unit can see it */
    int weak;
};

struct table {
    struct sym_ent *v;
    int n, cap;
};

static struct sym_ent *tab_find(struct table *t, const char *name)
{
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->v[i].name, name))
            return &t->v[i];
    return NULL;
}

static struct sym_ent *tab_add(struct table *t, const char *name,
                               const char *where)
{
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 256;
        t->v = xrealloc(t->v, (size_t)t->cap * sizeof *t->v);
    }
    struct sym_ent *e = &t->v[t->n++];
    memset(e, 0, sizeof *e);
    e->name = xstrndup(name, strlen(name));
    e->where = xstrndup(where, strlen(where));
    return e;
}

/* One relocatable object's symbol table, folded into the picture. */
static void scan_object(struct table *defs, struct table *refs,
                        const unsigned char *b, long len, const char *where)
{
    if (len < (long)sizeof(Elf64_Ehdr) || memcmp(b, "\177ELF", 4) != 0)
        return;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)b;
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr))
        return;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(b + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB)
            continue;
        const Elf64_Sym *sy = (const Elf64_Sym *)(b + sh[i].sh_offset);
        long nsym = (long)(sh[i].sh_size / sizeof(Elf64_Sym));
        const char *str = (const char *)(b + sh[sh[i].sh_link].sh_offset);
        for (long k = 1; k < nsym; k++) {
            const char *name = str + sy[k].st_name;
            if (!*name)
                continue;
            int bind = (int)ELF64_ST_BIND(sy[k].st_info);
            if (sy[k].st_shndx == SHN_UNDEF) {
                if (bind != STB_LOCAL && !tab_find(refs, name))
                    tab_add(refs, name, where);
            } else if (!tab_find(defs, name)) {
                struct sym_ent *e = tab_add(defs, name, where);
                e->defined = 1;
                e->local = bind == STB_LOCAL;
                e->weak = bind == STB_WEAK;
            }
        }
    }
}

/* An archive: every member, since the question is what COULD define the
 * symbol, not what the link happened to pull in. */
static void scan_archive(struct table *defs, struct table *refs,
                         const unsigned char *b, long len, const char *where)
{
    long off = 8;                              /* past "!<arch>\n" */
    while (off + 60 <= len) {
        const char *h = (const char *)b + off;
        char szbuf[11];
        memcpy(szbuf, h + 48, 10);
        szbuf[10] = 0;
        long size = strtol(szbuf, NULL, 10);
        char name[17];
        memcpy(name, h, 16);
        name[16] = 0;
        for (int i = 15; i >= 0 && (name[i] == ' ' || name[i] == '/'); i--)
            name[i] = 0;
        off += 60;
        if (size <= 0 || off + size > len)
            break;
        if (name[0] != '/' && strcmp(name, "__.SYMDEF") != 0) {
            char full[512];
            snprintf(full, sizeof full, "%s(%s)", where, name);
            scan_object(defs, refs, b + off, size, full);
        }
        off += size + (size & 1);
    }
}

static unsigned char *read_all(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = xmalloc((size_t)*len + 1);
    if (fread(b, 1, (size_t)*len, f) != (size_t)*len)
        *len = 0;
    fclose(f);
    return b;
}

/* ---- reading a C++ name ----
 *
 * Enough of the Itanium mangling to say which class and which function a
 * symbol belongs to: _ZN5Point3sumEv is Point::sum, _Z4mainv is main, and
 * _ZTV5Point is Point's vtable. Anything more elaborate (templates,
 * substitutions) is left alone rather than guessed at.
 */
static int demangle(const char *s, char *out, size_t cap)
{
    if (strncmp(s, "_Z", 2) != 0)
        return 0;
    const char *p = s + 2;
    const char *kind = NULL;
    if (!strncmp(p, "TV", 2)) { kind = "vtable for "; p += 2; }
    else if (!strncmp(p, "TI", 2)) { kind = "typeinfo for "; p += 2; }
    else if (!strncmp(p, "TS", 2)) { kind = "typeinfo name for "; p += 2; }
    int nested = *p == 'N';
    const char *cv = "";
    if (nested) {
        p++;
        /* CV-qualifiers ride between the N and the name: _ZNK5Shape4areaEv
         * is Shape::area() const, and missing them loses the name. */
        for (;;) {
            if (*p == 'K')      { cv = " const";    p++; }
            else if (*p == 'V') { cv = " volatile"; p++; }
            else if (*p == 'r') { p++; }
            else break;
        }
    }
    size_t n = 0;
    if (kind)
        n += (size_t)snprintf(out + n, cap - n, "%s", kind);
    int parts = 0;
    while (*p >= '1' && *p <= '9') {
        long l = strtol(p, (char **)&p, 10);
        if (l <= 0 || (long)strlen(p) < l)
            return 0;
        if (parts++)
            n += (size_t)snprintf(out + n, cap - n, "::");
        if (n + (size_t)l >= cap)
            return 0;
        memcpy(out + n, p, (size_t)l);
        n += (size_t)l;
        out[n] = 0;
        p += l;
    }
    if (!parts)
        return 0;
    if (nested && *p == 'E')
        p++;
    if (!kind && *p)                            /* a function */
        n += (size_t)snprintf(out + n, cap - n,
                              !strcmp(p, "v") ? "()" : "(...)");
    if (*cv)
        snprintf(out + n, cap - n, "%s", cv);
    return 1;
}

/* ---- the diagnosis ---- */

int doctor_run(char **inputs, int n)
{
    struct table defs = { NULL, 0, 0 }, refs = { NULL, 0, 0 };
    for (int i = 0; i < n; i++) {
        long len = 0;
        unsigned char *b = read_all(inputs[i], &len);
        if (!b || len <= 0) {
            fprintf(stderr, "embld: doctor: cannot read %s\n", inputs[i]);
            continue;
        }
        if (len >= 8 && !memcmp(b, "!<arch>\n", 8))
            scan_archive(&defs, &refs, b, len, inputs[i]);
        else
            scan_object(&defs, &refs, b, len, inputs[i]);
    }

    int bad = 0;
    for (int i = 0; i < refs.n; i++) {
        struct sym_ent *r = &refs.v[i];
        struct sym_ent *d = tab_find(&defs, r->name);
        if (d && !d->local)
            continue;                           /* it links: nothing to say */
        bad++;
        char pretty[512];
        int cxx = demangle(r->name, pretty, sizeof pretty);
        fprintf(stderr, "embld: undefined: %s\n", r->name);
        if (cxx)
            fprintf(stderr, "  that is %s\n", pretty);
        fprintf(stderr, "  wanted by %s\n", r->where);

        if (d && d->local) {
            fprintf(stderr, "  %s does define it — but as `static`, which "
                            "keeps it inside that\n  unit. Drop the `static`, "
                            "or move the caller into that file.\n", d->where);
            continue;
        }
        const char *hdr = header_declaring(r->name);
        if (hdr) {
            fprintf(stderr, "  it is the C library's %s (declared in %s): "
                            "link the library that\n  defines it (libc.a), "
                            "which a freestanding link does not add for you.\n",
                    r->name, hdr);
            continue;
        }
        if (!strncmp(r->name, "__cxa_", 6) || !strncmp(r->name, "_Unwind_", 8) ||
            strstr(r->name, "__cxxabiv1") || !strncmp(r->name, "_ZSt", 4)) {
            fprintf(stderr, "  it belongs to the C++ runtime: link libsupc++ "
                            "(and libstdc++ for the\n  library itself). A "
                            "freestanding link adds neither for you.\n");
            continue;
        }
        if (!strncmp(r->name, "_ZTV", 4) || !strncmp(r->name, "_ZTI", 4)) {
            fprintf(stderr, "  a class's vtable and typeinfo are emitted with "
                            "its KEY FUNCTION —\n  the first virtual function "
                            "that is not inline. If every virtual\n  function "
                            "of that class is inline or pure, no unit emits "
                            "them; if the\n  key function is merely declared, "
                            "define it.\n");
            continue;
        }
        if (cxx)
            fprintf(stderr, "  nothing here defines it. A member function "
                            "that is declared in the\n  class and never "
                            "defined looks exactly like this.\n");
        else
            fprintf(stderr, "  nothing on this command line defines it — is a "
                            "source file missing\n  from the link, or a "
                            "library?\n");
    }

    if (!bad) {
        printf("embld: doctor: every symbol referenced is defined "
               "(%d definitions across %d input%s)\n",
               defs.n, n, n == 1 ? "" : "s");
        return 0;
    }
    fprintf(stderr, "embld: doctor: %d symbol%s undefined\n", bad,
            bad == 1 ? "" : "s");
    return 1;
}
