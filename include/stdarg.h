/* EmbCC's stdarg.h.
 *
 * va_list is `char *` — deliberately the SAME type newlib's headers pick
 * for a non-GNU compiler (__VALIST becomes `char*` because EmbCC does not
 * define __GNUC__), so a va_list flows into vfprintf and friends with no
 * type conflict. va_start makes that pointer point at a real SysV
 * __va_list_tag (gp_offset/fp_offset/overflow_arg_area/reg_save_area)
 * that EmbCC builds on the frame, which is exactly what the libc built by
 * gcc expects to read. va_end is a no-op.
 *
 * va_arg walks the tag that va_start built (SysV's __va_list_tag, or the
 * AAPCS64 va_list record on aarch64) and advances it. va_copy gives the
 * destination a tag of its own, copied from the source's — so the two
 * lists advance independently. */
#ifndef _STDARG_H
#define _STDARG_H
#ifdef __cplusplus
/* C++: va_list is a type of its own, as g++'s (it takes part in
 * overloading and mangling; libstdc++ names __builtin_va_list), still
 * `char *` underneath in the C the C++ becomes */
typedef __builtin_va_list va_list;
typedef __builtin_va_list __gnuc_va_list;
#else
typedef char *va_list;
typedef char *__gnuc_va_list;
#endif
#define va_start(ap, last) __builtin_va_start((ap), (last))
#define va_arg(ap, type)   __builtin_va_arg((ap), type)
#define va_end(ap)         __builtin_va_end((ap))
#define va_copy(d, s)      __builtin_va_copy((d), (s))
#define __va_copy(d, s)    __builtin_va_copy((d), (s))
#endif
