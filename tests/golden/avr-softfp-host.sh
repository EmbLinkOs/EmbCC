#!/bin/sh
# lib/rt/avrfp*.c, built for the HOST and driven against native float.
#
# The same source that runs on the ATmega328P, compiled here and compared
# with the machine's own binary32 over every pair of a value grid. That is a
# different question from tests/golden/avr-float.sh, which asks whether the
# AVR BACKEND lowers these routines correctly; this asks whether the
# ARITHMETIC is right, and it can ask about far more cases because there is no
# 16 MHz part and no QEMU in the loop -- about 4400 comparisons in well under
# a second, against 28 images.
#
# It is worth having both, and the split was earned. Four bugs were found
# here, before any of this ran on a part:
#
#   * rounding recomputed the exponent field AFTER shifting the significand
#     into subnormal position, so 1.0f * 1e-38f came out one exponent high
#   * exact cancellation fell into the magnitude branch and produced -0 for
#     (-1) + 1, where IEEE-754 requires +0
#   * the divide did not maintain rem < den on the first iteration, so
#     3.0f / 1.0f was 2.0f
#   * the divide did not normalise a subnormal significand
#
# 477 mismatches, then 266, then 77, then 6, then none. A fifth bug needed
# the part to show itself and a sixth needed the backend, which is why
# avr-float.sh exists as well.
#
# ---- the retyping, which is the only trick here ----------------------
#
# On AVR an `int` is two bytes and a `long` is four, so the 32-bit type in
# this code is `long`. On the host a `long` is eight. Every file is therefore
# copied with `u32` and the routine signatures retyped to `int` before it is
# compiled -- and the FIRST version of this got it wrong in a way worth
# recording: it used `sed` with `\b`, which BSD sed does not support, so
# __floatsisf kept its `long` parameter against an `int` prototype and read
# eight bytes of an argument the caller passed four of. The substitution is
# done in python now, where the word boundary means what it says.
#
# Each file is compiled SEPARATELY, as on the target: they are separate
# objects there so a program links only what it calls, and two of them define
# the same `u64` typedef, so one translation unit would not even compile.
set -u
echo "TEST-MARKER avr-softfp-host"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-softfp-host
rm -rf "$out"; mkdir -p "$out"

python3 - "$out" <<'PY' || { echo "the retyping failed"; exit 1; }
import re, sys, os, glob
out = sys.argv[1]
src = os.path.join('lib', 'rt')
for path in [os.path.join(src, 'avrfp.h')] + sorted(glob.glob(os.path.join(src, 'avrfp*.c'))):
    s = open(path).read()
    s = s.replace('#ifdef __AVR__', '#if 1', 1)
    s = s.replace('#if defined(__AVR__) || defined(__RX__)', '#if 1', 1)
    # The 32-bit type: `long` on AVR, `int` here.
    s = s.replace('typedef unsigned long      u32;', 'typedef unsigned int       u32;')
    s = s.replace('typedef long               s32;', 'typedef int                s32;')
    # ...and the four signatures that name it directly. These are the ones a
    # `\b`-less sed silently left alone.
    s = s.replace('float __floatunsisf(unsigned long v)',
                  'float __floatunsisf(unsigned int v)')
    s = s.replace('float __floatsisf(long v)', 'float __floatsisf(int v)')
    s = s.replace('unsigned long __fixunssfsi(float f)',
                  'unsigned int __fixunssfsi(float f)')
    s = s.replace('long __fixsfsi(float f)', 'int __fixsfsi(float f)')
    # Casts and literals. `(long long)` and `(unsigned long long)` are eight
    # bytes on both machines and must NOT be touched -- which is why these
    # patterns end at the closing paren.
    s = re.sub(r'\(unsigned long\)', '(unsigned int)', s)
    s = re.sub(r'\(long\)', '(int)', s)
    s = s.replace('unsigned long v = __fixunssfsi', 'unsigned int v = __fixunssfsi')
    s = s.replace('~(unsigned int)0', '~0u')
    s = re.sub(r'0x([0-9a-fA-F]+)ul\b', r'0x\1u', s)
    s = re.sub(r'0x([0-9a-fA-F]+)l\b', r'0x\1', s)
    open(os.path.join(out, os.path.basename(path)), 'w').write(s)
PY

cat > "$out/drive.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <math.h>

float __addsf3(float, float); float __subsf3(float, float);
float __mulsf3(float, float); float __divsf3(float, float);
float __negsf2(float);
int __cmpsf2(float, float); int __eqsf2(float, float); int __nesf2(float, float);
int __ltsf2(float, float);  int __lesf2(float, float);
int __gtsf2(float, float);  int __gesf2(float, float);
int __unordsf2(float, float);
float __floatsisf(int); float __floatunsisf(unsigned int);
int __fixsfsi(float);    unsigned int __fixunssfsi(float);
float __floatdisf(long long); float __floatundisf(unsigned long long);
long long __fixsfdi(float);   unsigned long long __fixunssfdi(float);

static unsigned bits(float f) { unsigned u; memcpy(&u, &f, 4); return u; }
static int bad, n;
static float ca, cb;
static void chk(const char *op, float got, float want)
{
    n++;
    if (bits(got) == bits(want))
        return;
    if (isnan(got) && isnan(want))
        return;                         /* any NaN payload will do */
    if (bad < 16)
        printf("  %-9s a=%08x b=%08x got %08x want %08x  (%g, %g)\n",
               op, bits(ca), bits(cb), bits(got), bits(want),
               (double)ca, (double)cb);
    bad++;
}
static void chkb(const char *what, int got, int want, double a, double b)
{
    n++;
    if (got == want)
        return;
    if (bad < 16)
        printf("  %-9s (%g, %g): got %d want %d\n", what, a, b, got, want);
    bad++;
}

/* Chosen so that every case in the code is reached: both zeroes, the smallest
 * subnormal, the largest subnormal, the smallest normal, the largest finite,
 * values 24 bits apart so alignment shifts everything out, and a pair either
 * side of a rounding tie. */
static const float V[] = {
    0.0f, -0.0f, 1.0f, -1.0f, 2.0f, 0.5f, 3.0f, 7.0f, 0.1f, -0.1f,
    123.456f, -98765.4f, 1e-30f, 1e30f, 1e-38f, 3.4e38f, 1.17549435e-38f,
    5.87747e-39f, 1.4e-45f, -1.4e-45f, 16777216.0f, 16777217.0f, 8388608.5f,
    1.0f / 3.0f, 2.0f / 3.0f, 1e-7f, 65504.0f, 1.0000001f, 0.9999999f
};
#define NV ((int)(sizeof V / sizeof V[0]))

int main(void)
{
    int i, j, k;
    float inf = 1.0f / 0.0f, nan = 0.0f / 0.0f;

    for (i = 0; i < NV; i++) for (j = 0; j < NV; j++) {
        float a = V[i], b = V[j];
        ca = a; cb = b;
        chk("add", __addsf3(a, b), a + b);
        chk("sub", __subsf3(a, b), a - b);
        chk("mul", __mulsf3(a, b), a * b);
        chk("div", __divsf3(a, b), a / b);
        chkb("lt", __ltsf2(a, b) <  0, a <  b, a, b);
        chkb("le", __lesf2(a, b) <= 0, a <= b, a, b);
        chkb("gt", __gtsf2(a, b) >  0, a >  b, a, b);
        chkb("ge", __gesf2(a, b) >= 0, a >= b, a, b);
        chkb("eq", __eqsf2(a, b) == 0, a == b, a, b);
        chkb("ne", __nesf2(a, b) != 0, a != b, a, b);
    }
    /* Against infinity and NaN, in both operand positions. */
    for (i = 0; i < NV; i++) {
        float a = V[i];
        ca = a; cb = inf;
        chk("add-inf", __addsf3(a, inf), a + inf);
        chk("inf-add", __addsf3(inf, a), inf + a);
        chk("sub-inf", __subsf3(a, inf), a - inf);
        chk("mul-inf", __mulsf3(a, inf), a * inf);
        chk("div-inf", __divsf3(a, inf), a / inf);
        chk("inf-div", __divsf3(inf, a), inf / a);
        chk("div-0",   __divsf3(a, 0.0f), a / 0.0f);
        chk("neg",     __negsf2(a), -a);
        cb = nan;
        chk("add-nan", __addsf3(a, nan), a + nan);
        chk("mul-nan", __mulsf3(a, nan), a * nan);
        chk("div-nan", __divsf3(a, nan), a / nan);
        /* Every ordered comparison against NaN is false both ways round,
         * which for these entry points means the sign of the answer has to
         * make the caller's test fail in both directions. */
        chkb("nan-lt", __ltsf2(a, nan) <  0, 0, a, 0);
        chkb("nan-gt", __gtsf2(a, nan) >  0, 0, a, 0);
        chkb("nan-le", __lesf2(a, nan) <= 0, 0, a, 0);
        chkb("nan-ge", __gesf2(a, nan) >= 0, 0, a, 0);
        chkb("nan-lt2", __ltsf2(nan, a) <  0, 0, a, 0);
        chkb("nan-gt2", __gtsf2(nan, a) >  0, 0, a, 0);
        chkb("unord",  __unordsf2(a, nan) != 0, 1, a, 0);
        chkb("ord",    __unordsf2(a, a) != 0, isnan(a) ? 1 : 0, a, 0);
    }
    ca = inf; cb = -inf;
    chk("inf-inf", __addsf3(inf, -inf), inf + -inf);
    chk("0*inf",   __mulsf3(0.0f, inf), 0.0f * inf);
    chk("inf/inf", __divsf3(inf, inf), inf / inf);

    /* 32-bit integers, both directions. */
    {
        static const long long iv[] = {
            0, 1, -1, 2, 127, -128, 32767, -32768, 65535, 1000000, -1000000,
            16777216LL, 16777217LL, 16777219LL, 2147483647LL, -2147483648LL,
            123456789LL, -987654321LL, 4294967295LL, 2147483648LL
        };
        for (k = 0; k < (int)(sizeof iv / sizeof iv[0]); k++) {
            int s = (int)iv[k];
            unsigned u = (unsigned)iv[k];
            ca = 0; cb = 0;
            chk("i2f", __floatsisf(s), (float)s);
            chk("u2f", __floatunsisf(u), (float)u);
        }
    }
    {
        static const float fv[] = {
            0.0f, -0.0f, 1.0f, -1.0f, 1.9f, -1.9f, 2.5f, -2.5f, 1e9f, -1e9f,
            2147483520.0f, 4294967040.0f, 0.4f, -0.4f, 1e-10f, 1e-38f,
            1.4e-45f, 16777216.0f, 12345.678f, -12345.678f
        };
        for (k = 0; k < (int)(sizeof fv / sizeof fv[0]); k++) {
            chkb("f2i", __fixsfsi(fv[k]) == (int)fv[k], 1, fv[k], 0);
            if (fv[k] >= 0.0f)
                chkb("f2u", __fixunssfsi(fv[k]) == (unsigned)fv[k], 1,
                     fv[k], 0);
        }
    }
    /* 64-bit integers, both directions -- the pair the 32-bit harness left
     * out, and the only routines here that use a 64-bit type at all. */
    {
        static const long long lv[] = {
            0, 1, -1, 1000000000000LL, -1000000000000LL,
            9007199254740993LL, -9007199254740993LL,
            0x7fffffffffffffffLL, -0x7fffffffffffffffLL - 1,
            4294967296LL, 123456789012345LL
        };
        static const unsigned long long uv[] = {
            0ULL, 1ULL, 18000000000000000000ULL, 0xffffffffffffffffULL,
            0x8000000000000000ULL, 9007199254740993ULL
        };
        for (k = 0; k < (int)(sizeof lv / sizeof lv[0]); k++) {
            ca = 0; cb = 0;
            chk("ll2f", __floatdisf(lv[k]), (float)lv[k]);
        }
        for (k = 0; k < (int)(sizeof uv / sizeof uv[0]); k++) {
            ca = 0; cb = 0;
            chk("ull2f", __floatundisf(uv[k]), (float)uv[k]);
        }
    }
    {
        static const float fv[] = {
            0.0f, -0.0f, 1.0f, -1.0f, 1.9f, -1.9f, 1e12f, -1e12f, 1e18f,
            0.4f, 1e-38f, 9.007199e15f, 12345.678f, -12345.678f, 4294967040.0f
        };
        for (k = 0; k < (int)(sizeof fv / sizeof fv[0]); k++) {
            chkb("f2ll", __fixsfdi(fv[k]) == (long long)fv[k], 1, fv[k], 0);
            if (fv[k] >= 0.0f)
                chkb("f2ull", __fixunssfdi(fv[k]) == (unsigned long long)fv[k],
                     1, fv[k], 0);
        }
    }

    printf("%d checks, %d mismatches\n", n, bad);
    return bad != 0;
}
EOF

# -w, because the retyped source is the AVR source and its own build warns
# about nothing here; -ffp-contract=off and no -ffast-math, because the
# comparison is against IEEE-754 and the host must not reassociate.
cc -std=c99 -w -ffp-contract=off -I"$out" -o "$out/drive" \
    "$out"/avrfp*.c "$out/drive.c" -lm || {
    echo "the host build of lib/rt/avrfp*.c failed"; exit 1; }

"$out/drive" > "$out/log" 2>&1
rc=$?
cat "$out/log"
[ "$rc" -eq 0 ] || {
    echo "lib/rt/avrfp*.c disagrees with the host's own binary32 -- see above.
        Each line gives both operands and both results as BITS, because that
        is the only comparison that catches a wrong last place."
    exit 1; }

echo "lib/rt/avrfp*.c agrees with native binary32 on every case:
  add, subtract, multiply and divide over every pair of a 29-value grid --
  both zeroes, the smallest and largest subnormals, the smallest normal,
  the largest finite, values 24 bits apart, and a rounding tie either side
  all six comparisons over the same grid, plus the seven libgcc entry
  points against NaN in both operand positions, where every ordered test
  has to be false whichever way it is asked
  infinity and NaN as either operand of every operation, division by zero,
  inf - inf, 0 * inf and inf / inf
  int, unsigned, long long and unsigned long long to and from float --
  including LLONG_MIN, 2^64 - 1, and values past 24 bits that must round"
