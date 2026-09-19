/* The C++ front-end's one entry point (D-013): preprocessed C++ in, C out,
 * for the C front-end to compile. Everything else in src/cxx is internal
 * (cxx.h). */
#ifndef EMBCC_CXX_TRANSLATE_H
#define EMBCC_CXX_TRANSLATE_H

char *cxx_translate(const char *file, const char *src);

/* -fno-exceptions: no throw, try or catch, and no exception tables' worth
 * of cleanups (the default is on, as g++'s). */
void cxx_set_exceptions(int on);

/* What the preprocessor's __has_builtin answers in a C++ unit: the
 * builtin functions and type-trait intrinsics the front-end implements. */
int cxx_has_builtin(const char *name);

#endif
