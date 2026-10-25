/* <assert.h> — C11 §7.2.
 *
 * Deliberately without an include guard around the macro: §7.2p1 says the
 * header may be included more than once and that NDEBUG is consulted at
 * EACH inclusion, so a translation unit can turn assertions off for one
 * region and on again for the next. A guard would silently break that.
 */
#undef assert

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#if defined(__APPLE__)
/* Apple's C library reports a failed assertion through __assert_rtn,
 * with the arguments in its own order; __assert_fail is not there. */
#define assert(e) \
    ((e) ? (void)0 : __assert_rtn(__func__, __FILE__, __LINE__, #e))
#else
#define assert(e) \
    ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif
#endif

#ifndef _ASSERT_H_DECLS
#define _ASSERT_H_DECLS
#if defined(__APPLE__)
#ifdef __cplusplus
extern "C" [[noreturn]] void __assert_rtn(const char *func, const char *file,
                                          int line, const char *expr);
#else
_Noreturn void __assert_rtn(const char *func, const char *file, int line,
                            const char *expr);
#endif
#elif defined(__cplusplus)
extern "C" [[noreturn]] void __assert_fail(const char *expr, const char *file,
                                           int line, const char *func);
#else
_Noreturn void __assert_fail(const char *expr, const char *file, int line,
                             const char *func);
#endif
#endif

/* C11 §7.2p3: static_assert is a macro here, not a keyword. */
#ifndef __cplusplus
#define static_assert _Static_assert
#endif
