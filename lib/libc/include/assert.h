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
#define assert(e) \
    ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

#ifndef _ASSERT_H_DECLS
#define _ASSERT_H_DECLS
_Noreturn void __assert_fail(const char *expr, const char *file, int line,
                             const char *func);
#endif

/* C11 §7.2p3: static_assert is a macro here, not a keyword. */
#ifndef __cplusplus
#define static_assert _Static_assert
#endif
