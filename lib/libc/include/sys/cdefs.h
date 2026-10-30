/* <sys/cdefs.h> -- the BSD and glibc header macros ports include for */
#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS   }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#define __P(args)   args
#define __PMT(args) args
#define __THROW
#define __CONCAT(a, b) a##b
#define __STRING(x) #x
#define __unused     __attribute__((__unused__))
#define __dead2      __attribute__((__noreturn__))
#define __pure2      __attribute__((__const__))
#define __packed     __attribute__((__packed__))
#define __aligned(n) __attribute__((__aligned__(n)))
#define __printflike(f, a) __attribute__((__format__(__printf__, f, a)))
#define __nonnull(p) __attribute__((__nonnull__ p))
#define __wur        __attribute__((__warn_unused_result__))
#define __attribute_pure__  __attribute__((__pure__))
#define __attribute_const__ __attribute__((__const__))
#define __glibc_unlikely(c) __builtin_expect((c), 0)
#define __glibc_likely(c)   __builtin_expect((c), 1)

#endif
