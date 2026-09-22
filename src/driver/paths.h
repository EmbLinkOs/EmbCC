/* Where EmbCC's own files are.
 *
 * A compiler is not one binary: it is a binary plus the headers it
 * offers, the per-target libraries it links against, and the linker
 * script that lays its images out. Until now those were found only by
 * the person typing `-Ilib/libc/include`, which is why `embcc
 * hello.c` could not find <stdio.h> and why nothing was installable.
 *
 * ---- found relative to the binary, not baked in ------------------------
 *
 * The prefix is NOT compiled in. EmbCC asks the platform layer where it
 * is (plat_self_path) and resolves everything from there, which gives
 * one behaviour on every host:
 *
 *   an unpacked tarball works from wherever it was unpacked
 *   a build tree works without being installed at all
 *   `make install PREFIX=...` needs no rebuild, and neither does moving
 *   the result afterwards
 *
 * A baked-in prefix would have to be chosen before the compiler was
 * built and would be wrong the moment anything moved -- and on Windows,
 * where there is no /usr/local to agree on, it would be wrong from the
 * start.
 *
 * ---- the two layouts ---------------------------------------------------
 *
 *   installed    <prefix>/bin/embcc
 *                <prefix>/lib/embcc/<version>/include/
 *                <prefix>/lib/embcc/<version>/include/c++/
 *                <prefix>/lib/embcc/<version>/<triple>/{libc.a,crt1.o,link.ld}
 *
 *   build tree   <root>/embcc
 *                <root>/lib/libc/include/
 *                <root>/lib/libcxx/include/
 *                <root>/build/libc/<dir>/{libc.a,crt1.o}
 *
 * Both are recognised, because a compiler that behaves differently
 * before and after `make install` is a compiler whose tests prove
 * nothing about what people run.
 *
 * EMBCC_PREFIX overrides the search entirely, which is how a test
 * exercises an installed layout without installing anything.
 */
#ifndef EMBCC_DRIVER_PATHS_H
#define EMBCC_DRIVER_PATHS_H

#include <stddef.h>

/* The directory holding this compiler's support files -- the versioned
 * directory when installed, the source root in a build tree, or NULL
 * when neither could be found (a binary carried away from its files).
 * Callers must treat NULL as "no defaults", not as an error: an
 * explicit -I still works and is how EmbCC has always been used. */
const char *paths_lib_dir(void);

/* The default system include directories, in search order, or NULL when
 * there are none. `*n` receives the count. These are added as if by
 * -isystem, AFTER every -I the user gave: a project's own header must
 * win over ours, or a program cannot override anything we ship. */
const char *const *paths_default_includes(int *n);

/* A per-target support file -- paths_target_file("x86_64-linux-gnu",
 * "crt1.o", buf, sizeof buf). 1 when it is there, 0 when it is not, so
 * a caller can say WHICH file is missing rather than reporting a link
 * failure fifty undefined symbols later.
 *
 * The caller supplies the buffer on purpose. This returned a pointer to
 * a static one first, and the very first caller asked for two files in
 * a row -- which made both names the second file, so crt1.o was never
 * linked and the image had no _start. A function whose natural use is
 * its misuse is the function's fault. */
int paths_target_file(const char *triple, const char *name,
                      char *out, size_t cap);

/* `--print-search-dirs`: what the compiler decided, in the form a
 * person can check against `ls`. A search path that cannot be inspected
 * is a search path nobody can debug. */
void paths_print_search_dirs(void);

#endif
