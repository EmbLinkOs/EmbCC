/* The C++ front-end's one entry point (D-013): preprocessed C++ in, C out,
 * for the C front-end to compile. Everything else in src/cxx is internal
 * (cxx.h). */
#ifndef EMBCC_CXX_TRANSLATE_H
#define EMBCC_CXX_TRANSLATE_H

char *cxx_translate(const char *file, const char *src);

#endif
