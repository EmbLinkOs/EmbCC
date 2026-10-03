#!/bin/sh
# AVR: IEEE-754 binary32, in software.
#
# This target has no floating-point unit and no binary64 either: `float` AND
# `double` are both four-byte binary32, which is avr-gcc's documented default
# and what src/arch/target.c's data model says. So every float operation is a
# call, and lib/rt/avrfp*.c is what it calls.
#
# ---- judged against the host, bit for bit ----------------------------
#
# Every value is printed as its FOUR BYTES in hex, not as a number. There is
# no printf here to print it with, and bytes are the stronger check anyway:
# binary32 is binary32 on both machines, so a difference of one bit in the
# last place shows up -- and that is exactly the class of bug this code had.
# The host's own float arithmetic is the oracle.
#
# The inputs are bit patterns too, through a union, rather than decimal
# literals. That is not fussiness: it keeps an image from linking a
# conversion routine it is not testing (see below), and it means the two
# machines start from identical bits rather than from two front ends'
# readings of "1e-38".
#
# The routines were validated on the host first -- same source, driven against
# native float over 3338 cases -- which is how four rounding bugs were found
# before any of this ran on the part: the exponent recomputed after shifting
# into subnormal position, exact cancellation taking the magnitude path and
# producing -0, a divide that did not maintain rem < den (3.0f / 1.0f = 2.0f),
# and a divide that did not normalise a subnormal significand. A fifth was
# found here, on the part: a multiply that folded the low word of the product
# into sticky, so 7.0f * 1.4e-45f came out zero.
#
# ---- SEVEN images, because the part has 32 KB ------------------------
#
# Software binary32 is about 40 KB of AVR text at -O0, on a part with 32768
# bytes of flash. It does not have to fit at once: the routines are in
# separate objects -- lib/rt/avrfp.c is only unpack and round, and
# avrfpadd.c, avrfpmul.c, avrfpdiv.c, avrfpcmp.c, avrfpi.c, avrfpi64.c and
# avrfpfix64.c are one group each -- so nothing links what it does not call.
#
# So this test is one image per object, each linking the core plus the one
# group it exercises. That is not a workaround for the size, it is the
# structure being tested: if a group had a dependency it should not have, its
# image would fail to link, which is how the multiply's call to __mulsi3 was
# found. Seven images at four optimisation levels is twenty-eight runs on the
# part, and that is what it costs.
#
# The boundaries are where they are because -O0 put them there. addsub was in
# the core until its image was 9 KB bigger than the part allowed, and the
# 64-bit conversions were one object until the two directions together came to
# 19 KB. Neither is over at -O1 or above; the split is sized for the level
# that generates the most code, because that is the level someone debugging
# builds at.
#
# The size is asserted before each is run, because an over-size image
# presents at run time as a call past the end of flash, and that is a poor
# way to learn it.
set -u
echo "TEST-MARKER avr-float"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-float
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
H=$out/h; mkdir -p "$H"

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: no $QEMU"; exit 0; }

cat > "$out/pr.h" <<'EOF'
void writec(int c);
void puts_(const char *s);

static void phex(unsigned char b)
{
    static const char d[] = "0123456789abcdef";
    writec(d[(b >> 4) & 15]);
    writec(d[b & 15]);
}
/* A float as its four bytes, most significant first. Not a decimal print:
 * there is no printf here, and the bytes ARE the value -- an ulp of
 * difference shows, where a rounded decimal would hide it. */
static void p32(float v)
{
    unsigned char *p = (unsigned char *)&v;
    int i;
    for (i = 3; i >= 0; i--)
        phex(p[i]);
    writec(' ');
}
static void pbytes(const void *p, int n)
{
    const unsigned char *b = (const unsigned char *)p;
    int i;
    for (i = n - 1; i >= 0; i--)
        phex(b[i]);
    writec(' ');
}
static void pd(int v) { writec(v ? '1' : '0'); writec(' '); }

/* A float from its bits, a byte at a time so it does not depend on how wide
 * `unsigned long` is (four bytes here, eight on the host). A union READ is
 * not a conversion, so this pulls in no runtime routine -- which is the
 * point: an image tests one group and links one group. */
static float K(unsigned long u)
{
    union { unsigned char b[4]; float f; } v;
    v.b[0] = (unsigned char)u;
    v.b[1] = (unsigned char)(u >> 8);
    v.b[2] = (unsigned char)(u >> 16);
    v.b[3] = (unsigned char)(u >> 24);
    return v.f;
}

#define K_ONE                0x3f800000ul   /* 1.0f */
#define K_TWO                0x40000000ul   /* 2.0f */
#define K_THREE              0x40400000ul   /* 3.0f */
#define K_FOUR               0x40800000ul   /* 4.0f */
#define K_TEN                0x41200000ul   /* 10.0f */
#define K_HALF               0x3f000000ul   /* 0.5f */
#define K_P1                 0x3dcccccdul   /* 0.1f */
#define K_P2                 0x3e4ccccdul   /* 0.2f */
#define K_ONEP5              0x3fc00000ul   /* 1.5f */
#define K_ONEP9              0x3ff33333ul   /* 1.9f */
#define K_E30                0x7149f2caul   /* 1e30f */
#define K_EM30               0x0da24260ul   /* 1e-30f */
#define K_BIG                0x7f7fc99eul   /* 3.4e38f, just under the top */
#define K_SMALL              0x006ce3eeul   /* 1e-38f, a subnormal */
#define K_EM10               0x2edbe6fful   /* 1e-10f */
#define K_EM40               0x000116c2ul   /* 1e-40f, a subnormal */
#define K_SUBMIN             0x00000001ul   /* 1.4e-45f, the smallest there is */
#define K_12345              0x4640e400ul   /* 12345.0f */
#define K_NEG2E9             0xceee6b28ul   /* -2000000000.0f */
#define K_U4E9               0x4f6e6b28ul   /* 4000000000.0f, past LONG_MAX */
#define K_E12                0x5368d4a5ul   /* 1e12f */
#define K_NEGE12             0xd368d4a5ul   /* -1e12f */
#define K_E18                0x5d5e0b6bul   /* 1e18f, past LLONG_MAX signed */

/* The special values, and the negatives, as BITS.
 *
 * lib/rt/avrfpadd.c's image below makes each of these by arithmetic -- inf by
 * overflow, NaN from inf - inf, -0 by negating +0 -- because producing them is
 * part of what add and subtract have to get right. Every other image writes
 * them down instead, for two reasons. It keeps the image linking one group:
 * building inf with an add would pull 9 KB of addsub into the divide's image,
 * which on this part is the difference between fitting and not. And it keeps
 * the test honest: a divide that mishandles infinity should fail the divide's
 * image, not be masked by an add that produced the wrong infinity. */
#define K_ZERO               0x00000000ul
#define K_NEGZERO            0x80000000ul
#define K_INF                0x7f800000ul
#define K_NEGINF             0xff800000ul
#define K_NAN                0x7fc00000ul   /* the quiet NaN this code makes */
#define K_SEVEN              0x40e00000ul   /* 7.0f */
#define K_NEGONE             0xbf800000ul
#define K_NEGTWO             0xc0000000ul
#define K_NEGONEP9           0xbff33333ul   /* -1.9f */
#define K_NEGSMALL           0x806ce3eeul   /* -1e-38f, a negative subnormal */
#define K_NEG12345           0xc640e400ul
EOF

# ---- the six programs -------------------------------------------------
# Each goes through FUNCTIONS, so the optimizer cannot fold the test at
# compile time and leave the runtime unexercised.

cat > "$out/fadd.c" <<'EOF'
#include "pr.h"
static float add(float a, float b) { return a + b; }
static float sub(float a, float b) { return a - b; }
static float neg(float a)          { return -a; }
void run(void)
{
    float one = K(K_ONE), two = K(K_TWO), half = K(K_HALF);
    float big = K(K_BIG), small = K(K_SMALL), sub1 = K(K_SUBMIN);
    float zero = sub(one, one);
    float inf = add(big, big);          /* made by overflow, not written */
    float nan = sub(inf, inf);

    p32(add(one, two));
    p32(add(K(K_P1), K(K_P2)));         /* the classic inexact pair */
    p32(add(K(K_E30), K(K_EM30)));      /* the small one vanishes entirely */
    p32(add(neg(one), one));            /* exact cancellation: +0, never -0 */
    p32(sub(one, one));
    p32(sub(half, one));
    p32(add(sub1, sub1));               /* subnormal + subnormal */
    p32(add(small, neg(small)));
    p32(sub(K(K_SMALL), sub1));         /* a subnormal difference */
    p32(neg(zero));                     /* -0 */
    p32(add(zero, neg(zero)));          /* +0, by the sign rule for sums */
    p32(add(inf, one));
    p32(sub(inf, inf));                 /* NaN out of two infinities */
    p32(add(nan, one));                 /* NaN propagates */
    p32(add(big, big));                 /* overflow to inf */
    p32(neg(inf));
    p32(add(one, K(K_EM40)));           /* the subnormal disappears */
    puts_("DONE\n");
}
EOF

cat > "$out/fmul.c" <<'EOF'
#include "pr.h"
static float mul(float a, float b) { return a * b; }
void run(void)
{
    float one = K(K_ONE), two = K(K_TWO), seven = K(K_SEVEN);
    float big = K(K_BIG), small = K(K_SMALL), sub1 = K(K_SUBMIN);
    float zero = K(K_ZERO), inf = K(K_INF);

    p32(mul(K(K_ONEP5), K(K_ONEP5)));
    p32(mul(seven, sub1));       /* a subnormal operand: this was 0 until the
                                  * significands were normalised before the
                                  * product, because the whole 48-bit result
                                  * landed in the low word and was folded into
                                  * the sticky bit */
    p32(mul(sub1, seven));       /* and the other way round */
    p32(mul(big, two));          /* overflows to inf */
    p32(mul(small, K(K_EM10)));  /* underflows into the subnormals */
    p32(mul(small, seven));
    p32(mul(small, small));      /* all the way to zero */
    p32(mul(K(K_NEGONE), zero)); /* -0 */
    p32(mul(zero, zero));
    p32(mul(K(K_NEGZERO), K(K_NEGZERO)));   /* +0 */
    p32(mul(inf, zero));         /* NaN */
    p32(mul(inf, two));
    p32(mul(K(K_NEGINF), two));
    p32(mul(K(K_NAN), two));     /* NaN propagates */
    p32(mul(K(K_P1), K(K_P1)));
    p32(mul(K(K_P1), K(K_TEN))); /* not 1.0f, and the host agrees it is not */
    p32(mul(K(K_E12), K(K_E12)));
    p32(mul(sub1, sub1));
    p32(mul(K(K_NEGSMALL), K(K_NEGSMALL)));
    { /* halved until it falls off the bottom */
      float t = one; int i;
      for (i = 0; i < 50; i++) t = mul(t, K(K_HALF));
      p32(t); }
    puts_("DONE\n");
}
EOF

cat > "$out/fdiv.c" <<'EOF'
#include "pr.h"
static float div_(float a, float b) { return a / b; }
void run(void)
{
    float one = K(K_ONE), three = K(K_THREE);
    float big = K(K_BIG), small = K(K_SMALL);
    float zero = K(K_ZERO), inf = K(K_INF);

    p32(div_(three, one));       /* gave 2.0f until the pre-scale kept
                                  * rem < den on the first iteration */
    p32(div_(one, three));
    p32(div_(one, K(K_TWO)));
    p32(div_(K(K_TEN), K(K_FOUR)));
    p32(div_(one, big));
    p32(div_(big, small));       /* overflows to inf */
    p32(div_(small, big));       /* underflows to zero */
    p32(div_(one, zero));        /* inf, not a trap */
    p32(div_(K(K_NEGONE), zero));/* -inf */
    p32(div_(one, K(K_NEGZERO)));/* -inf, from the divisor's sign */
    p32(div_(zero, zero));       /* NaN */
    p32(div_(inf, inf));         /* NaN */
    p32(div_(one, inf));
    p32(div_(inf, one));
    p32(div_(K(K_NAN), one));    /* NaN propagates */
    p32(div_(K(K_EM40), K(K_TWO)));  /* a subnormal dividend */
    p32(div_(small, small));     /* a subnormal by itself: exactly 1.0f */
    p32(div_(K(K_SUBMIN), K(K_TWO)));/* the smallest there is, halved: 0 */
    p32(div_(K(K_P1), K(K_P2)));
    p32(div_(K(K_NEGSMALL), K(K_SMALL)));
    puts_("DONE\n");
}
EOF

cat > "$out/fcmp.c" <<'EOF'
#include "pr.h"
static int lt(float a, float b) { return a <  b; }
static int le(float a, float b) { return a <= b; }
static int gt(float a, float b) { return a >  b; }
static int ge(float a, float b) { return a >= b; }
static int eq(float a, float b) { return a == b; }
static int ne(float a, float b) { return a != b; }
void run(void)
{
    float one = K(K_ONE), two = K(K_TWO), big = K(K_BIG);
    float zero = K(K_ZERO), inf = K(K_INF), nan = K(K_NAN);
    float sub1 = K(K_SUBMIN);

    pd(lt(one, two)); pd(lt(two, one)); pd(lt(one, one));
    pd(le(one, one)); pd(le(two, one));
    pd(gt(two, one)); pd(gt(one, two));
    pd(ge(one, one)); pd(ge(one, two));
    pd(eq(one, one)); pd(ne(one, two)); pd(ne(one, one));
    puts_("| ");
    pd(eq(zero, K(K_NEGZERO)));         /* +0 == -0, per IEEE-754 */
    pd(lt(K(K_NEGZERO), zero));
    pd(lt(K(K_NEGONE), one));           /* across the sign */
    pd(lt(K(K_NEGTWO), K(K_NEGONE)));   /* both negative: the order reverses */
    pd(lt(zero, sub1));                 /* a subnormal is above zero */
    pd(lt(K(K_NEGSMALL), zero));
    pd(lt(big, inf));
    pd(gt(inf, big)); pd(eq(inf, inf)); pd(lt(K(K_NEGINF), inf));
    puts_("| ");
    /* NaN: every ordered test is false in BOTH directions, and != is true */
    pd(lt(nan, one)); pd(gt(nan, one)); pd(le(nan, one)); pd(ge(nan, one));
    pd(lt(one, nan)); pd(gt(one, nan)); pd(le(one, nan)); pd(ge(one, nan));
    pd(eq(nan, nan)); pd(ne(nan, nan));
    pd(le(nan, nan)); pd(ge(nan, nan));
    pd(lt(nan, inf)); pd(gt(nan, K(K_NEGINF)));
    puts_("DONE\n");
}
EOF

cat > "$out/fcvt.c" <<'EOF'
#include "pr.h"
static float i2f(int v)            { return (float)v; }
static float l2f(long v)           { return (float)v; }
static float u2f_(unsigned long v) { return (float)v; }
static long  f2l(float v)          { return (long)v; }
static unsigned long f2u_(float v) { return (unsigned long)v; }
static int   f2i(float v)          { return (int)v; }
void run(void)
{
    long l; unsigned long u; int n;

    p32(i2f(0)); p32(i2f(1)); p32(i2f(-1));
    p32(i2f(12345)); p32(i2f(-12345)); p32(i2f(32767)); p32(i2f(-32768));
    p32(l2f(0L)); p32(l2f(2000000000L)); p32(l2f(-2000000000L));
    p32(l2f(16777216L));         /* exactly 24 bits: still exact */
    p32(l2f(16777217L));         /* one more: has to round, to 16777216 */
    p32(l2f(-16777217L));
    p32(l2f(2147483647L));       /* LONG_MAX here: rounds up to 2^31 */
    p32(u2f_(4000000000UL));     /* past LONG_MAX: the unsigned entry point */
    p32(u2f_(0xfffffffful));
    puts_("| ");
    l = f2l(K(K_ONEP9));     pbytes(&l, 4);   /* truncates toward zero */
    l = f2l(K(K_NEGONEP9));  pbytes(&l, 4);   /* ...in both directions */
    l = f2l(K(K_ZERO));      pbytes(&l, 4);
    l = f2l(K(K_NEGZERO));   pbytes(&l, 4);
    l = f2l(K(K_12345));     pbytes(&l, 4);
    l = f2l(K(K_NEG12345));  pbytes(&l, 4);
    l = f2l(K(K_NEG2E9));    pbytes(&l, 4);
    l = f2l(K(K_SMALL));     pbytes(&l, 4);   /* a subnormal: 0 */
    u = f2u_(K(K_U4E9));     pbytes(&u, 4);   /* past LONG_MAX, unsigned */
    u = f2u_(K(K_ONEP9));    pbytes(&u, 4);
    /* (int) is a narrower cast -- two bytes here, four on the host -- so the
     * result is widened to `long` before it is printed. The widening is an
     * integer operation; the cast under test is still float -> int. */
    n = f2i(K(K_12345));     l = n; pbytes(&l, 4);
    n = f2i(K(K_NEGONE));    l = n; pbytes(&l, 4);
    n = f2i(K(K_SMALL));     l = n; pbytes(&l, 4);   /* a subnormal: 0 */
    puts_("DONE\n");
}
EOF

cat > "$out/fcvt64.c" <<'EOF'
#include "pr.h"
static float ll2f(long long v)           { return (float)v; }
static float ull2f(unsigned long long v) { return (float)v; }
void run(void)
{
    p32(ll2f(0LL)); p32(ll2f(1LL)); p32(ll2f(-1LL));
    p32(ll2f(1000000000000LL));
    p32(ll2f(-1000000000000LL));
    p32(ll2f(16777216LL));           /* exactly 24 bits: exact */
    p32(ll2f(16777217LL));           /* one more: rounds */
    p32(ll2f(9007199254740993LL));   /* far past 24 bits */
    p32(ll2f(-9007199254740993LL));
    p32(ll2f(0x7fffffffffffffffLL)); /* LLONG_MAX: rounds up to 2^63 */
    p32(ll2f(-0x7fffffffffffffffLL - 1));  /* LLONG_MIN: exactly -2^63 */
    p32(ull2f(0ULL)); p32(ull2f(1ULL));
    p32(ull2f(18000000000000000000ULL));   /* past LLONG_MAX */
    p32(ull2f(0xffffffffffffffffULL));     /* rounds up to 2^64 */
    puts_("DONE\n");
}
EOF

cat > "$out/ffix64.c" <<'EOF'
#include "pr.h"
static long long f2ll(float v)           { return (long long)v; }
static unsigned long long f2ull(float v) { return (unsigned long long)v; }
void run(void)
{
    long long s; unsigned long long t;

    s = f2ll(K(K_ONEP9));    pbytes(&s, 8);   /* truncates toward zero */
    s = f2ll(K(K_NEGONEP9)); pbytes(&s, 8);   /* ...in both directions */
    s = f2ll(K(K_ZERO));     pbytes(&s, 8);
    s = f2ll(K(K_NEGZERO));  pbytes(&s, 8);
    s = f2ll(K(K_ONE));      pbytes(&s, 8);
    s = f2ll(K(K_E12));      pbytes(&s, 8);
    s = f2ll(K(K_NEGE12));   pbytes(&s, 8);
    s = f2ll(K(K_12345));    pbytes(&s, 8);
    s = f2ll(K(K_NEG12345)); pbytes(&s, 8);
    s = f2ll(K(K_SMALL));    pbytes(&s, 8);   /* a subnormal: 0 */
    s = f2ll(K(K_P1));       pbytes(&s, 8);   /* below 1: also 0 */
    t = f2ull(K(K_E18));     pbytes(&t, 8);   /* past LLONG_MAX, unsigned */
    t = f2ull(K(K_ONEP9));   pbytes(&t, 8);
    t = f2ull(K(K_U4E9));    pbytes(&t, 8);
    puts_("DONE\n");
}
EOF

# The sign and classification builtins. They read and write the bits and
# call nothing, so this image links no runtime at all. AVR refused every
# one of them ("cannot lower bitcast"): a float is four bytes in the
# general registers there, and the conversion to its bits is a copy.
cat > "$out/fbits.c" <<'EOF'
#include "pr.h"
static float fab(float v)           { return __builtin_fabsf(v); }
static double fabd(double v)        { return __builtin_fabs(v); }
static float cps(float a, float b)  { return __builtin_copysignf(a, b); }
void run(void)
{
    p32(fab(K(K_NEGZERO))); p32(fab(K(K_NEG12345))); p32(fab(K(K_NEGINF)));
    p32(fab(K(K_NEGSMALL))); p32((float)fabd((double)K(K_NEGONEP9)));
    p32(cps(K(K_12345), K(K_NEGZERO))); p32(cps(K(K_NEG12345), K(K_ZERO)));
    p32(cps(K(K_NAN), K(K_NEGONE)));
    puts_("| ");
    pd(__builtin_signbit(K(K_NEGZERO))); pd(__builtin_signbit(K(K_ZERO)));
    pd(__builtin_signbit(K(K_NEGINF)));
    pd(__builtin_isnan(K(K_NAN))); pd(__builtin_isnan(K(K_INF)));
    pd(__builtin_isinf(K(K_NEGINF))); pd(__builtin_isinf(K(K_BIG)));
    pd(__builtin_isfinite(K(K_BIG))); pd(__builtin_isfinite(K(K_NAN)));
    pd(__builtin_isnormal(K(K_ONE))); pd(__builtin_isnormal(K(K_SMALL)));
    pd(__builtin_isnormal(K(K_ZERO)));
    pd(__builtin_isinf_sign(K(K_NEGINF)) < 0);
    pd(__builtin_isinf_sign(K(K_INF)) > 0);
    puts_("DONE\n");
}
EOF

cat > "$out/hostio.c" <<'EOF'
/* The same two routines on the host, so the comparison is textual. */
#include <stdio.h>
void writec(int c) { putchar(c); }
void puts_(const char *s) { while (*s) writec(*s++); }
int main(void) { void run(void); run(); fflush(stdout); return 0; }
EOF

cat > "$H/main.c" <<'EOF'
void run(void);
int main(void) { run(); for (;;) ; }
EOF

"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" || {
    echo "the startup did not assemble"; exit 1; }

# Which float objects each image needs -- and these lists are part of the
# test, not just the link line. avrfpcmp.c needs NOTHING else: a comparison
# reads the exponent and significand fields and never unpacks or rounds. The
# others want avrfp.o for __avrfp_unpack and __avrfp_round, and only fadd
# wants avrfpadd.o. If any list were wrong, the link would say so by name --
# which is how the multiply's dependency on __mulsi3 was found.
progs="fadd fmul fdiv fcmp fcvt fcvt64 ffix64 fbits"
fbits_rt=""
fadd_rt="avrfp avrfpadd"
# fmul also needs lib/rt/avr.c: mul24 splits its operands into 12-bit halves
# and multiplies them with `*`, which on this target is a call to __mulsi3.
# That is deliberate -- an inline 48-bit shift-and-add measured ten times the
# code -- and anything that multiplies floats was going to link avr.c anyway.
fmul_rt="avrfp avrfpmul avr"
fdiv_rt="avrfp avrfpdiv"
fcmp_rt="avrfpcmp"
fcvt_rt="avrfp avrfpi"
fcvt64_rt="avrfp avrfpi64"
ffix64_rt="avrfp avrfpfix64"

for which in $progs; do
    cc -std=c99 -w -I"$out" -o "$out/host.$which" "$out/$which.c" \
        "$out/hostio.c" || { echo "$which: the host build failed"; exit 1; }
    "$out/host.$which" > "$out/want.$which" || {
        echo "$which: the host program failed"; exit 1; }
done

for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$H/io.o" || exit 1
    "$EMBCC" --target=avr $O -c "$H/main.c" -o "$H/main.o" || exit 1
    for which in $progs; do
        eval "need=\$${which}_rt"
        rm -f "$H"/rtfp*.o
        for f in $need; do
            "$EMBCC" --target=avr $O -c "lib/rt/$f.c" -o "$H/rtfp_$f.o" || {
                echo "$O: lib/rt/$f.c did not compile"; exit 1; }
        done
        "$EMBCC" --target=avr $O -I"$out" -c "$out/$which.c" -o "$H/run.o" \
            2> "$out/c.err" || {
            echo "$O $which: did not compile:"; head -6 "$out/c.err"; exit 1; }
        EMBCC_AVR_HARNESS="$H" sh tests/harness/avr/link.sh "$H/run.elf" \
            "$H/run.o" "$H/main.o" 2> "$out/l.err" || {
            echo "$O $which: link failed:"; head -4 "$out/l.err"; exit 1; }
        if command -v llvm-size >/dev/null 2>&1; then
            tx=$(llvm-size "$H/run.elf" 2>/dev/null | awk 'NR==2{print $1}')
            if [ -n "$tx" ] && [ "$tx" -gt 32768 ]; then
                echo "$O $which: the image is $tx bytes of text and this part
        has 32768. Software binary32 is about 33 KB all told, which is why
        the routines are in separate objects and each image here links only
        the one group it tests."
                exit 1
            fi
        fi
        # EMBCC_QEMU_UNTIL=DONE ends each run the moment the image prints
        # its sentinel, instead of when the timeout expires. The image never
        # exits on its own -- a reset handler is the bottom of the stack on a
        # bare part -- so without this every one of these twenty-eight runs
        # costs the FULL timeout however fast it was: at tests/run.sh's
        # exported 20 seconds that is nine minutes of waiting for programs
        # that each finish in under one, and it made this the slowest test in
        # the suite by a factor of ten. The timeout is still the bound for an
        # image that never gets there.
        EMBCC_QEMU_UNTIL=DONE \
        EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
            sh tests/harness/avr/run.sh "$H/run.elf" \
            > "$out/got.$which$O" 2>/dev/null
        sed -n '1,/DONE/p' "$out/got.$which$O" > "$out/cut"
        grep -q DONE "$out/cut" || {
            echo "$O $which: did not reach its sentinel. What it printed:"
            head -c 300 "$out/got.$which$O" | od -c | head -6; exit 1; }
        cmp -s "$out/want.$which" "$out/cut" || {
            echo "$O $which: the ATmega328P disagrees with the host, bit for bit."
            echo "  want: $(cat "$out/want.$which")"
            echo "  got:  $(cat "$out/cut")"
            python3 - "$out/want.$which" "$out/cut" <<'PY' 2>/dev/null || true
import sys
w=open(sys.argv[1]).read().split(); g=open(sys.argv[2]).read().split()
for n,(a,b) in enumerate(zip(w,g)):
    if a!=b:
        print("  first difference at field %d: want %s got %s" % (n+1,a,b)); break
else:
    print("  the fields agree; the difference is in length (%d vs %d)"
          % (len(w),len(g)))
PY
            exit 1; }
    done
done

echo "software binary32 agrees with the host BIT FOR BIT on a real
ATmega328P, eight images at four optimisation levels:
  add and subtract -- exact cancellation (which must give +0, never -0), a
  subnormal sum and a subnormal difference, an operand that vanishes
  entirely, overflow to inf, NaN from inf - inf, and NaN propagation
  multiply -- a subnormal operand in either position (which gave zero until
  the significands were normalised before the product), overflow to inf,
  underflow into the subnormals and then to zero, -0, inf * 0, and a value
  halved fifty times until it falls off the bottom
  divide -- 3.0f / 1.0f (which gave 2.0f until the pre-scale kept rem < den),
  division by zero in both signs, 0/0, inf/inf, and a subnormal dividend
  compare -- all six operators on ordered pairs, both negative so the order
  reverses, +0 == -0, a subnormal against zero, and every operator against
  NaN in BOTH directions, where each one has to be false
  convert -- int, long and unsigned long to and from float, truncation
  toward zero on both signs, a value past LONG_MAX through the unsigned
  entry point, and 16777217 which cannot be represented and has to round
  a 64-bit integer to a float -- long long and unsigned long long, LLONG_MAX
  and LLONG_MIN, 2^64 - 1 which rounds up to 2^64, and 9007199254740993
  a float to a 64-bit integer -- truncation toward zero on both signs, a
  subnormal and a value below 1 (both zero), and past LLONG_MAX unsigned
  the sign and classification builtins -- fabs of -0, -inf and a negative
  subnormal, copysign onto a NaN, signbit, isnan, isinf, isfinite,
  isnormal and isinf_sign on the special values"
