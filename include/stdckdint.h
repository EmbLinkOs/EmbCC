/* <stdckdint.h> — C23 §7.20: checked integer arithmetic.
 *
 * ckd_add(r, a, b) stores a + b into *r, wrapped to *r's type, and is true
 * when the exact result did not fit -- whatever the three integer types
 * are, mixed signedness included, because the sum is computed as if with
 * infinite range first. ckd_sub and ckd_mul likewise. They are the
 * compiler's overflow builtins, which EmbCC lowers to the operation and a
 * test of its carry or overflow flag: no library code, and a freestanding
 * header, as C23 requires.
 */
#ifndef _STDCKDINT_H
#define _STDCKDINT_H

#define __STDC_VERSION_STDCKDINT_H__ 202311L

#define ckd_add(r, a, b) ((_Bool)__builtin_add_overflow((a), (b), (r)))
#define ckd_sub(r, a, b) ((_Bool)__builtin_sub_overflow((a), (b), (r)))
#define ckd_mul(r, a, b) ((_Bool)__builtin_mul_overflow((a), (b), (r)))

#endif
