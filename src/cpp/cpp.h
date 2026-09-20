/* The preprocessor (ARCHITECTURE §2, §5): a text -> text pass run
 * before the lexer. Output carries line markers (# LINE "FILE") that
 * the lexer consumes, so every diagnostic still lands on the line the
 * human wrote, not the line the expansion produced.
 *
 * Implemented: #include ("..." and <...> via -I paths), #define
 * (object- and function-like, # stringize, ## paste), #undef,
 * #if/#ifdef/#ifndef/#elif/#else/#endif with a constant-expression
 * evaluator (incl. defined()), #error, #pragma (ignored), __FILE__,
 * __LINE__, and the predefined x86_64-elf macro table from M0.
 */
#ifndef EMBCC_CPP_CPP_H
#define EMBCC_CPP_CPP_H

/* Preprocesses the file at `path` (its content in `src`), returning
 * malloc'd expanded text. incdirs/nincdirs are the -I search paths.
 * Exits with a diagnostic on any preprocessing error. */
char *cpp_process(const char *path, const char *src,
                  const char **incdirs, int nincdirs);

/* A C++ unit: what __has_builtin answers (the C++ front-end's builtins,
 * type traits included), and whether exceptions are on (__cpp_exceptions,
 * __EXCEPTIONS). Set before cpp_process; C units leave them alone, and
 * C's preprocessing does not change. */
void cpp_set_cxx(int (*has_builtin)(const char *name), int exceptions);
/* C++'s -std=: the year (1998, 2011, 2014, 2017, 2020, 2023, 2026) and
 * whether c++NN rather than gnu++NN (__STRICT_ANSI__) */
void cpp_set_cxx_std(int year, int strict);
void cpp_set_cxx_char8(int on);             /* -fchar8_t */
/* -DNAME[=VALUE] (undef 0) or -UNAME (undef 1), before cpp_process */
void cpp_cmdline_define(const char *text, int undef);

/* Which include directories are system ones (-isystem, the compiler's own):
 * their headers are what -MM leaves out of the dependency list. `flags` is
 * indexed as incdirs is, and must outlive the preprocessing. */
void cpp_set_system_dirs(const int *flags, int n);

/* The headers the last cpp_process opened, in order (-M and friends). */
int cpp_dep_count(void);
const char *cpp_dep_path(int i);
int cpp_dep_is_system(int i);

#endif
