/* <stdbit.h>'s functions, C23 §7.18, as functions: for a pointer to one,
 * or a call written (stdc_count_ones_ui)(x). A call by name is the
 * header's own inline helper (include/stdbit.h), which these are too --
 * one definition of each operation. */
#define __STDBIT_NO_MACROS
#include <stdbit.h>

#define DEF(N, H, R)                                                        \
    R N##_uc(unsigned char x)       { return __stdc_##H##_uc(x); }         \
    R N##_us(unsigned short x)      { return __stdc_##H##_us(x); }         \
    R N##_ui(unsigned int x)        { return __stdc_##H##_ui(x); }         \
    R N##_ul(unsigned long x)       { return __stdc_##H##_ul(x); }         \
    R N##_ull(unsigned long long x) { return __stdc_##H##_ull(x); }
__STDBIT_EACH(DEF)

unsigned int stdc_bit_width_uc(unsigned char x)  { return __stdc_bw_uc(x); }
unsigned int stdc_bit_width_us(unsigned short x) { return __stdc_bw_us(x); }
unsigned int stdc_bit_width_ui(unsigned int x)   { return __stdc_bw_ui(x); }
unsigned int stdc_bit_width_ul(unsigned long x)  { return __stdc_bw_ul(x); }
unsigned int stdc_bit_width_ull(unsigned long long x)
{
    return __stdc_bw_ull(x);
}

unsigned char stdc_bit_floor_uc(unsigned char x)   { return __stdc_bf_uc(x); }
unsigned short stdc_bit_floor_us(unsigned short x) { return __stdc_bf_us(x); }
unsigned int stdc_bit_floor_ui(unsigned int x)     { return __stdc_bf_ui(x); }
unsigned long stdc_bit_floor_ul(unsigned long x)   { return __stdc_bf_ul(x); }
unsigned long long stdc_bit_floor_ull(unsigned long long x)
{
    return __stdc_bf_ull(x);
}

unsigned char stdc_bit_ceil_uc(unsigned char x)   { return __stdc_bc_uc(x); }
unsigned short stdc_bit_ceil_us(unsigned short x) { return __stdc_bc_us(x); }
unsigned int stdc_bit_ceil_ui(unsigned int x)     { return __stdc_bc_ui(x); }
unsigned long stdc_bit_ceil_ul(unsigned long x)   { return __stdc_bc_ul(x); }
unsigned long long stdc_bit_ceil_ull(unsigned long long x)
{
    return __stdc_bc_ull(x);
}
