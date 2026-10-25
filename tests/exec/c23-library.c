/* C23's library: <stdbit.h> and <stdckdint.h> (freestanding, the
 * compiler's own headers), memccpy and memset_explicit, free_sized and
 * memalignment, strfromd, mbrtoc8 and c8rtomb, timespec_getres, roundeven,
 * nextup and the fmaximum family, printf's %B and #b and its wN length
 * modifiers, scanf's %b, the 0b prefix in strtol and %i, <stdint.h>'s
 * widths, and a variadic assert. And what mbrtoc32 and c16rtomb got
 * wrong where wchar_t is 16 bits (Xtensa): a char32_t holds all of
 * Unicode, and they cut it to what a wchar_t holds. */
// expect-exit: 42
// needs-libc: C23 functions the harnesses' newlib has not got
#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbit.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uchar.h>

static int bits(void)
{
    if (stdc_leading_zeros((unsigned char)1) != 7 ||
        stdc_leading_zeros((unsigned short)1) != 15 ||
        stdc_leading_zeros(1u) != sizeof(unsigned) * CHAR_BIT - 1 ||
        stdc_leading_zeros(0ull) != 64)
        return 1;
    if (stdc_trailing_zeros(8u) != 3 ||
        stdc_trailing_ones((unsigned char)0x07) != 3 ||
        stdc_leading_ones((unsigned char)0xF0) != 4 ||
        stdc_leading_ones(~0ul) != sizeof(long) * CHAR_BIT)
        return 2;
    if (stdc_first_leading_one((unsigned char)0x10) != 4 ||
        stdc_first_leading_zero((unsigned char)0xF0) != 5 ||
        stdc_first_trailing_one(0x10u) != 5 ||
        stdc_first_trailing_zero((unsigned char)0xFF) != 0 ||
        stdc_first_leading_one(0u) != 0)
        return 3;
    if (stdc_count_ones(0xF0F0ull) != 8 ||
        stdc_count_zeros((unsigned short)0xFFFF) != 0 ||
        !stdc_has_single_bit(64u) || stdc_has_single_bit((unsigned char)0) ||
        stdc_has_single_bit(6u))
        return 4;
    if (stdc_bit_width(0u) != 0 || stdc_bit_width(255u) != 8 ||
        stdc_bit_floor(100u) != 64 || stdc_bit_ceil(100u) != 128 ||
        stdc_bit_ceil((unsigned char)200) != 0 || stdc_bit_ceil(0u) != 1 ||
        stdc_bit_ceil(1ull) != 1 || stdc_bit_floor(0ul) != 0)
        return 5;
    /* the functions themselves, through a pointer */
    unsigned (*ones)(unsigned) = stdc_count_ones_ui;
    unsigned char (*ceil8)(unsigned char) = stdc_bit_ceil_uc;
    if (ones(0xFFu) != 8 || ceil8(3) != 4)
        return 6;
    union { unsigned u; unsigned char b[sizeof(unsigned)]; } e = { 1 };
    if ((__STDC_ENDIAN_NATIVE__ == __STDC_ENDIAN_LITTLE__) != (e.b[0] == 1))
        return 7;

    int r;
    unsigned char uc;
    long long ll;
    if (!ckd_add(&r, INT_MAX, 1) || r != INT_MIN ||
        ckd_mul(&r, 1000, 1000) || r != 1000000)
        return 8;
    if (!ckd_sub(&uc, 3, 5) || uc != 254 || ckd_add(&ll, -1, 2u) || ll != 1)
        return 9;
    return 0;
}

static int memory(void)
{
    char d[8];
    if (memccpy(d, "abc;def", ';', 8) != d + 4 || memcmp(d, "abc;", 4) != 0 ||
        memccpy(d, "abcdef", ';', 6) != NULL)
        return 10;
    char key[16] = "secret!";
    if (memset_explicit(key, 0, sizeof key) != key || key[0] || key[15])
        return 11;
    void *p = malloc(10);
    free_sized(p, 10);
    void *q = aligned_alloc(64, 64);
    if (!q || memalignment(q) < 64 || memalignment((char *)q + 4) != 4 ||
        memalignment(NULL) != 0 || memalignment((void *)0x1000) != 0x1000)
        return 12;
    free_aligned_sized(q, 64, 64);

    char b[32];
    if (strfromd(b, sizeof b, "%.3f", 3.14159) != 5 || strcmp(b, "3.142") ||
        strfromf(b, sizeof b, "%g", 0.5f) != 3 || strcmp(b, "0.5") ||
        strfroml(b, sizeof b, "%.1e", 1.25L) != 7 || strcmp(b, "1.2e+00") ||
        strfromd(b, sizeof b, "%d", 1.0) != -1)
        return 13;
    struct timespec t, res;
    if ((timespec_get(&t, TIME_UTC) == TIME_UTC) !=
        (timespec_getres(&res, TIME_UTC) == TIME_UTC))
        return 14;
    return 0;
}

static int units(void)
{
    mbstate_t st;
    unsigned char c8 = 0;
    char out[8];
    memset(&st, 0, sizeof st);
    if (mbrtoc8(&c8, "\xc3\xa9x", 3, &st) != 2 || c8 != 0xC3)
        return 20;
    if (mbrtoc8(&c8, "x", 1, &st) != (size_t)-3 || c8 != 0xA9)
        return 21;                      /* the owed unit, nothing consumed */
    if (mbrtoc8(&c8, "x", 1, &st) != 1 || c8 != 'x')
        return 22;
    if (c8rtomb(out, 0xE2, &st) != 0 || c8rtomb(out, 0x82, &st) != 0 ||
        c8rtomb(out, 0xAC, &st) != 3 || memcmp(out, "\xe2\x82\xac", 3) != 0)
        return 23;
    if (c8rtomb(out, 0x80, &st) != (size_t)-1)
        return 24;                      /* a continuation with no start */

    /* all of Unicode in a char32_t and in a surrogate pair, whatever
     * wchar_t is */
    char32_t c32 = 0;
    char16_t c16 = 0;
    memset(&st, 0, sizeof st);
    if (mbrtoc32(&c32, "\xf0\x9f\x98\x80", 4, &st) != 4 || c32 != 0x1F600)
        return 25;
    if (c32rtomb(out, 0x1F600, &st) != 4 ||
        memcmp(out, "\xf0\x9f\x98\x80", 4) != 0)
        return 26;
    if (mbrtoc16(&c16, "\xf0\x9f\x98\x80", 4, &st) != 4 || c16 != 0xD83D ||
        mbrtoc16(&c16, "", 1, &st) != (size_t)-3 || c16 != 0xDE00)
        return 27;
    if (c16rtomb(out, 0xD83D, &st) != 0 || c16rtomb(out, 0xDE00, &st) != 4 ||
        memcmp(out, "\xf0\x9f\x98\x80", 4) != 0)
        return 28;
    return 0;
}

static int same(double a, double b)
{
    return a == b && signbit(a) == signbit(b);
}

static int maths(void)
{
    if (roundeven(2.5) != 2 || roundeven(3.5) != 4 || roundeven(-2.5) != -2 ||
        !same(roundeven(-0.5), -0.0) || roundeven(2.4) != 2 ||
        roundevenf(0.5f) != 0 || roundevenl(1.5L) != 2 ||
        roundeven(1e300) != 1e300)
        return 30;
    if (nextup(1.0) != 1.0 + DBL_EPSILON ||
        nextdown(1.0) != 1.0 - DBL_EPSILON / 2 ||
        nextupf(-0.0f) != FLT_TRUE_MIN ||
        nextdownl(LDBL_TRUE_MIN) != 0 || nextupl(LDBL_MAX) != INFINITY)
        return 31;
    double nan = NAN;
    if (!same(fmaximum(-0.0, 0.0), 0.0) || !same(fminimum(0.0, -0.0), -0.0) ||
        !isnan(fmaximum(nan, 1.0)) || !isnan(fminimum(1.0, nan)) ||
        fmaximum(1.0, 2.0) != 2 || fminimumf(1.0f, 2.0f) != 1)
        return 32;
    if (fmaximum_num(nan, 1.0) != 1 || fminimum_num(-1.0, nan) != -1 ||
        !isnan(fmaximum_num(nan, nan)))
        return 33;
    if (fmaximum_mag(-3.0, 2.0) != -3 || fminimum_mag(-3.0, 2.0) != 2 ||
        fmaximum_mag(-2.0, 2.0) != 2 || fminimum_mag(-2.0, 2.0) != -2 ||
        fminimum_mag_num(nan, -1.0) != -1 || fmaximum_mag_numl(4.0L, nan) != 4)
        return 34;
    if (exp10(2.0) != 100 || exp10f(3.0f) != 1000)
        return 35;
    return 0;
}

static int formats(void)
{
    char b[96];
    snprintf(b, sizeof b, "%B|%#b|%#B|%#b|%w8d|%w16u|%w32x|%w64d|%wf8d|%wf32u",
             5u, 5u, 5u, 0u, (int8_t)-3, (uint16_t)65535,
             (uint32_t)0xdeadbeef, INT64_MIN, (int_fast8_t)-5,
             (uint_fast32_t)7);
    if (strcmp(b, "101|0b101|0B101|0|-3|65535|deadbeef|"
                  "-9223372036854775808|-5|7") != 0)
        return 40;
    unsigned u = 0;
    int i1 = 0, i2 = 0;
    int16_t h = 0;
    uint64_t q = 0;
    int_fast16_t fh = 0;
    if (sscanf("101 0b11 -0b1 -300 ffffffffffffffff 12", "%b %i %i %w16d %w64x %wf16d",
               &u, &i1, &i2, &h, &q, &fh) != 6 ||
        u != 5 || i1 != 3 || i2 != -1 || h != -300 || q != UINT64_MAX ||
        fh != 12)
        return 41;

    char *e;
    const char *z = "0xz";
    if (strtol("0b101", &e, 0) != 5 || *e || strtol("0b101", &e, 2) != 5 ||
        strtoul("0B11", NULL, 0) != 3)
        return 42 + 1;
    if (strtol(z, &e, 16) != 0 || e != z + 1 || strtol("0b2", &e, 0) != 0 ||
        *e != 'b')
        return 44;                      /* a prefix with no digit is its 0 */
    if (INT32_WIDTH != 32 || UINTPTR_WIDTH != sizeof(uintptr_t) * CHAR_BIT ||
        SIZE_WIDTH != sizeof(size_t) * CHAR_BIT || UINT_LEAST8_WIDTH != 8)
        return 45;
    struct pt { int x, y; };
    assert((struct pt){1, 2}.x == 1);   /* one expression, with commas */
    return 0;
}

int main(void)
{
    int r;
    if ((r = bits()) || (r = memory()) || (r = units()) || (r = maths()) ||
        (r = formats()))
        return r;
    return 42;
}
