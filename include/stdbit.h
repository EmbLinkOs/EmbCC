/* <stdbit.h> — C23 §7.18: the bits of the unsigned integer types.
 *
 * Fourteen operations -- leading and trailing zeros and ones, the first
 * leading or trailing zero or one, the counts of zeros and ones, whether
 * one bit is set, the bit width, and the powers of two below and above --
 * each for unsigned char (_uc), short (_us), int (_ui), long (_ul) and
 * long long (_ull), and type-generic over all five.
 *
 * A freestanding header, as C23 requires: a firmware build with no C
 * library has it. Each named function is also a macro (C11 7.1.4 allows
 * one for any library function) over an inline helper made of the
 * compiler's bit builtins, so a call costs what the instruction does --
 * CLZ on a Cortex-M3, a short sequence where there is none. The functions
 * themselves, for a pointer to one or a call through (name)(x), are in
 * EmbCC's runtime (lib/rt/stdbit.c), which every program links.
 */
#ifndef _STDBIT_H
#define _STDBIT_H

#define __STDC_VERSION_STDBIT_H__ 202311L

#define __STDC_ENDIAN_LITTLE__ __ORDER_LITTLE_ENDIAN__
#define __STDC_ENDIAN_BIG__    __ORDER_BIG_ENDIAN__
#define __STDC_ENDIAN_NATIVE__ __BYTE_ORDER__

#ifdef __cplusplus
extern "C" {
#endif

#define __STDBIT_W(T) ((unsigned int)(sizeof(T) * __CHAR_BIT__))

/* The helpers, for one type: T, its suffix, and the builtins that count in
 * a word of at least its width -- int's for the three narrow types, with
 * the extra high bits of the word taken off a leading count. */
#define __STDBIT_HELPERS(S, T, CLZ, CTZ, POP, WT)                            \
static __inline__ unsigned int __stdc_lz_##S(T x)                           \
{ return x ? (unsigned int)CLZ((WT)x) - (__STDBIT_W(WT) - __STDBIT_W(T))    \
           : __STDBIT_W(T); }                                               \
static __inline__ unsigned int __stdc_tz_##S(T x)                           \
{ return x ? (unsigned int)CTZ((WT)x) : __STDBIT_W(T); }                    \
static __inline__ unsigned int __stdc_pop_##S(T x)                          \
{ return (unsigned int)POP((WT)x); }                                        \
static __inline__ unsigned int __stdc_lo_##S(T x)                           \
{ return __stdc_lz_##S((T)~x); }                                            \
static __inline__ unsigned int __stdc_to_##S(T x)                           \
{ return __stdc_tz_##S((T)~x); }                                            \
static __inline__ unsigned int __stdc_flz_##S(T x)                          \
{ return (T)~x ? __stdc_lo_##S(x) + 1 : 0; }                                \
static __inline__ unsigned int __stdc_flo_##S(T x)                          \
{ return x ? __stdc_lz_##S(x) + 1 : 0; }                                    \
static __inline__ unsigned int __stdc_ftz_##S(T x)                          \
{ return (T)~x ? __stdc_to_##S(x) + 1 : 0; }                                \
static __inline__ unsigned int __stdc_fto_##S(T x)                          \
{ return x ? __stdc_tz_##S(x) + 1 : 0; }                                    \
static __inline__ unsigned int __stdc_cz_##S(T x)                           \
{ return __STDBIT_W(T) - __stdc_pop_##S(x); }                               \
static __inline__ _Bool __stdc_one_##S(T x)                                 \
{ return x && !(T)(x & (T)(x - 1)); }                                       \
static __inline__ unsigned int __stdc_bw_##S(T x)                           \
{ return __STDBIT_W(T) - __stdc_lz_##S(x); }                                \
static __inline__ T __stdc_bf_##S(T x)                                      \
{ return x ? (T)((T)1 << (__stdc_bw_##S(x) - 1)) : 0; }                     \
/* the least power of two not below x; 0 where that does not fit T */       \
static __inline__ T __stdc_bc_##S(T x)                                      \
{ unsigned int w = x > 1 ? __stdc_bw_##S((T)(x - 1)) : 0;                   \
  return w < __STDBIT_W(T) ? (T)((T)1 << w) : 0; }

__STDBIT_HELPERS(uc, unsigned char, __builtin_clz, __builtin_ctz,
                 __builtin_popcount, unsigned int)
__STDBIT_HELPERS(us, unsigned short, __builtin_clz, __builtin_ctz,
                 __builtin_popcount, unsigned int)
__STDBIT_HELPERS(ui, unsigned int, __builtin_clz, __builtin_ctz,
                 __builtin_popcount, unsigned int)
__STDBIT_HELPERS(ul, unsigned long, __builtin_clzl, __builtin_ctzl,
                 __builtin_popcountl, unsigned long)
__STDBIT_HELPERS(ull, unsigned long long, __builtin_clzll, __builtin_ctzll,
                 __builtin_popcountll, unsigned long long)

/* The functions, each a name, its helper, and what it returns. */
#define __STDBIT_EACH(X)                                                    \
    X(stdc_leading_zeros, lz, unsigned int)                                 \
    X(stdc_leading_ones, lo, unsigned int)                                  \
    X(stdc_trailing_zeros, tz, unsigned int)                                \
    X(stdc_trailing_ones, to, unsigned int)                                 \
    X(stdc_first_leading_zero, flz, unsigned int)                           \
    X(stdc_first_leading_one, flo, unsigned int)                            \
    X(stdc_first_trailing_zero, ftz, unsigned int)                          \
    X(stdc_first_trailing_one, fto, unsigned int)                           \
    X(stdc_count_zeros, cz, unsigned int)                                   \
    X(stdc_count_ones, pop, unsigned int)                                   \
    X(stdc_has_single_bit, one, _Bool)

#define __STDBIT_DECL(N, H, R)                                              \
    R N##_uc(unsigned char value);                                          \
    R N##_us(unsigned short value);                                         \
    R N##_ui(unsigned int value);                                           \
    R N##_ul(unsigned long value);                                          \
    R N##_ull(unsigned long long value);
__STDBIT_EACH(__STDBIT_DECL)
unsigned int stdc_bit_width_uc(unsigned char value);
unsigned int stdc_bit_width_us(unsigned short value);
unsigned int stdc_bit_width_ui(unsigned int value);
unsigned int stdc_bit_width_ul(unsigned long value);
unsigned int stdc_bit_width_ull(unsigned long long value);
unsigned char stdc_bit_floor_uc(unsigned char value);
unsigned short stdc_bit_floor_us(unsigned short value);
unsigned int stdc_bit_floor_ui(unsigned int value);
unsigned long stdc_bit_floor_ul(unsigned long value);
unsigned long long stdc_bit_floor_ull(unsigned long long value);
unsigned char stdc_bit_ceil_uc(unsigned char value);
unsigned short stdc_bit_ceil_us(unsigned short value);
unsigned int stdc_bit_ceil_ui(unsigned int value);
unsigned long stdc_bit_ceil_ul(unsigned long value);
unsigned long long stdc_bit_ceil_ull(unsigned long long value);

#ifndef __STDBIT_NO_MACROS     /* lib/rt/stdbit.c defines the functions */
#define stdc_leading_zeros_uc(x)  __stdc_lz_uc(x)
#define stdc_leading_zeros_us(x)  __stdc_lz_us(x)
#define stdc_leading_zeros_ui(x)  __stdc_lz_ui(x)
#define stdc_leading_zeros_ul(x)  __stdc_lz_ul(x)
#define stdc_leading_zeros_ull(x) __stdc_lz_ull(x)
#define stdc_leading_ones_uc(x)  __stdc_lo_uc(x)
#define stdc_leading_ones_us(x)  __stdc_lo_us(x)
#define stdc_leading_ones_ui(x)  __stdc_lo_ui(x)
#define stdc_leading_ones_ul(x)  __stdc_lo_ul(x)
#define stdc_leading_ones_ull(x) __stdc_lo_ull(x)
#define stdc_trailing_zeros_uc(x)  __stdc_tz_uc(x)
#define stdc_trailing_zeros_us(x)  __stdc_tz_us(x)
#define stdc_trailing_zeros_ui(x)  __stdc_tz_ui(x)
#define stdc_trailing_zeros_ul(x)  __stdc_tz_ul(x)
#define stdc_trailing_zeros_ull(x) __stdc_tz_ull(x)
#define stdc_trailing_ones_uc(x)  __stdc_to_uc(x)
#define stdc_trailing_ones_us(x)  __stdc_to_us(x)
#define stdc_trailing_ones_ui(x)  __stdc_to_ui(x)
#define stdc_trailing_ones_ul(x)  __stdc_to_ul(x)
#define stdc_trailing_ones_ull(x) __stdc_to_ull(x)
#define stdc_first_leading_zero_uc(x)  __stdc_flz_uc(x)
#define stdc_first_leading_zero_us(x)  __stdc_flz_us(x)
#define stdc_first_leading_zero_ui(x)  __stdc_flz_ui(x)
#define stdc_first_leading_zero_ul(x)  __stdc_flz_ul(x)
#define stdc_first_leading_zero_ull(x) __stdc_flz_ull(x)
#define stdc_first_leading_one_uc(x)  __stdc_flo_uc(x)
#define stdc_first_leading_one_us(x)  __stdc_flo_us(x)
#define stdc_first_leading_one_ui(x)  __stdc_flo_ui(x)
#define stdc_first_leading_one_ul(x)  __stdc_flo_ul(x)
#define stdc_first_leading_one_ull(x) __stdc_flo_ull(x)
#define stdc_first_trailing_zero_uc(x)  __stdc_ftz_uc(x)
#define stdc_first_trailing_zero_us(x)  __stdc_ftz_us(x)
#define stdc_first_trailing_zero_ui(x)  __stdc_ftz_ui(x)
#define stdc_first_trailing_zero_ul(x)  __stdc_ftz_ul(x)
#define stdc_first_trailing_zero_ull(x) __stdc_ftz_ull(x)
#define stdc_first_trailing_one_uc(x)  __stdc_fto_uc(x)
#define stdc_first_trailing_one_us(x)  __stdc_fto_us(x)
#define stdc_first_trailing_one_ui(x)  __stdc_fto_ui(x)
#define stdc_first_trailing_one_ul(x)  __stdc_fto_ul(x)
#define stdc_first_trailing_one_ull(x) __stdc_fto_ull(x)
#define stdc_count_zeros_uc(x)  __stdc_cz_uc(x)
#define stdc_count_zeros_us(x)  __stdc_cz_us(x)
#define stdc_count_zeros_ui(x)  __stdc_cz_ui(x)
#define stdc_count_zeros_ul(x)  __stdc_cz_ul(x)
#define stdc_count_zeros_ull(x) __stdc_cz_ull(x)
#define stdc_count_ones_uc(x)  __stdc_pop_uc(x)
#define stdc_count_ones_us(x)  __stdc_pop_us(x)
#define stdc_count_ones_ui(x)  __stdc_pop_ui(x)
#define stdc_count_ones_ul(x)  __stdc_pop_ul(x)
#define stdc_count_ones_ull(x) __stdc_pop_ull(x)
#define stdc_has_single_bit_uc(x)  __stdc_one_uc(x)
#define stdc_has_single_bit_us(x)  __stdc_one_us(x)
#define stdc_has_single_bit_ui(x)  __stdc_one_ui(x)
#define stdc_has_single_bit_ul(x)  __stdc_one_ul(x)
#define stdc_has_single_bit_ull(x) __stdc_one_ull(x)
#define stdc_bit_width_uc(x)  __stdc_bw_uc(x)
#define stdc_bit_width_us(x)  __stdc_bw_us(x)
#define stdc_bit_width_ui(x)  __stdc_bw_ui(x)
#define stdc_bit_width_ul(x)  __stdc_bw_ul(x)
#define stdc_bit_width_ull(x) __stdc_bw_ull(x)
#define stdc_bit_floor_uc(x)  __stdc_bf_uc(x)
#define stdc_bit_floor_us(x)  __stdc_bf_us(x)
#define stdc_bit_floor_ui(x)  __stdc_bf_ui(x)
#define stdc_bit_floor_ul(x)  __stdc_bf_ul(x)
#define stdc_bit_floor_ull(x) __stdc_bf_ull(x)
#define stdc_bit_ceil_uc(x)  __stdc_bc_uc(x)
#define stdc_bit_ceil_us(x)  __stdc_bc_us(x)
#define stdc_bit_ceil_ui(x)  __stdc_bc_ui(x)
#define stdc_bit_ceil_ul(x)  __stdc_bc_ul(x)
#define stdc_bit_ceil_ull(x) __stdc_bc_ull(x)
#endif

/* The type-generic forms, over the five types (an unsigned bit-field or
 * enum promotes to one of them first). */
#define __STDBIT_G(H, x) _Generic((x),                                      \
    unsigned char: __stdc_##H##_uc,                                         \
    unsigned short: __stdc_##H##_us,                                        \
    unsigned int: __stdc_##H##_ui,                                          \
    unsigned long: __stdc_##H##_ul,                                         \
    unsigned long long: __stdc_##H##_ull)(x)

#define stdc_leading_zeros(x)       __STDBIT_G(lz, x)
#define stdc_leading_ones(x)        __STDBIT_G(lo, x)
#define stdc_trailing_zeros(x)      __STDBIT_G(tz, x)
#define stdc_trailing_ones(x)       __STDBIT_G(to, x)
#define stdc_first_leading_zero(x)  __STDBIT_G(flz, x)
#define stdc_first_leading_one(x)   __STDBIT_G(flo, x)
#define stdc_first_trailing_zero(x) __STDBIT_G(ftz, x)
#define stdc_first_trailing_one(x)  __STDBIT_G(fto, x)
#define stdc_count_zeros(x)         __STDBIT_G(cz, x)
#define stdc_count_ones(x)          __STDBIT_G(pop, x)
#define stdc_has_single_bit(x)      __STDBIT_G(one, x)
#define stdc_bit_width(x)           __STDBIT_G(bw, x)
#define stdc_bit_floor(x)           __STDBIT_G(bf, x)
#define stdc_bit_ceil(x)            __STDBIT_G(bc, x)

#ifdef __cplusplus
}
#endif

#endif
