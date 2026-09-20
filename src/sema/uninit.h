/* -Wuninitialized / -Wmaybe-uninitialized: reading a local before anything
 * wrote it (docs/TOOLING.md T4). */
#ifndef EMBCC_UNINIT_H
#define EMBCC_UNINIT_H

#include "../parse/ast.h"

/* One frame slot, as semantic analysis knows it. The array is indexed by
 * var_index, so it parallels the scope's table exactly. */
struct uninit_var {
    const char *name;
    struct type *ty;
    int is_param;       /* a parameter arrives initialized */
    int is_static;      /* static storage: zero-initialized before main */
    int line, col;      /* the declaration, for the note */
};

/* Walk f's body and report every local read on a path that never wrote
 * it. `file` is the file the diagnostics name. */
void uninit_check(const char *file, struct func *f,
                  const struct uninit_var *vars, int nvars);

#endif
