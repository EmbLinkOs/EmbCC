#!/bin/sh
# Apple's arm64 calling convention and data model, RUN: half of each
# program from clang, half from EmbCC (--target=aarch64-apple-darwin),
# linked by the system and executed natively, both directions, -O0 and
# -O2. macOS on Apple silicon only -- elsewhere there is no machine to
# run it on.
#
# Apple departs from AAPCS64 where a convention only shows itself when
# someone else's code is on the other side, and EmbCC had taken the
# standard everywhere:
#
#   * named arguments on the stack take their own size and alignment:
#     f(8 ints, char, short, int) puts them at sp+0, +2 and +4, not in
#     eight-byte slots (variadic ones keep eight);
#   * nothing rounds to an even register -- not an __int128, not a
#     16-aligned struct;
#   * plain char is signed, wchar_t is int, long double is double;
#   * va_list is the walking pointer itself, so va_copy is assignment
#     (copying 32 bytes as for AAPCS64's record copied the arguments).
#
# Each of those made the old compiler's half disagree with clang's.
set -u
echo "TEST-MARKER darwin-abi"
. "$(dirname "$0")/../lib.sh"

[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || {
    echo "skipped: needs macOS on arm64 to run Apple's convention"; exit 0; }
command -v cc > /dev/null 2>&1 || { echo "skipped: no system cc"; exit 0; }

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/darwin-abi
rm -rf "${out:?}"; mkdir -p "$out"

cat > "$out/abi.h" << 'E'
#include <stdarg.h>
struct c3 { char a, b, c; };
struct s12 { int a, b, c; };
struct hf3 { float x, y, z; };
struct hd { double x, y; };
struct __attribute__((aligned(16))) a16 { long x; };
struct n16 { __int128 v; };
long pk(int a, int b, int c, int d, int e, int f, int g, int h,
        char x, short y, int z, long long w, char v);
double fl(double a, double b, double c, double d, double e, double f,
          double g, double h, float i, float j, double k, float l);
double hfa(double a, double b, double c, double d, double e, double f,
           double g, double h, float i, struct hf3 s, float j, struct hd t);
long cs(int a, int b, int c, int d, int e, int f, int g, int h, char x,
        struct c3 s, char y, struct s12 t, short z);
long al(int a, int b, int c, int d, int e, int f, int g, int h, char x,
        struct a16 s, char y);
long i128(int a, __int128 v, int b);
long n128(int a, struct n16 v, int b);
int ext(signed char c, short s, unsigned char u, unsigned short w);
signed char rc(int x);
unsigned short rus(int x);
long double ldf(long double a, double b, long double c);
long vs(int a, int b, int c, int d, int e, int f, int g, int h, char x,
        short y, ...);
struct al16 { long x; } __attribute__((aligned(16)));
struct hf2 { float a, b; };
struct big24 { long a, b, c; };
long vstruct(int n, ...);
long vcopy(int n, ...);
E

cat > "$out/callee.c" << 'E'
#include "abi.h"
long pk(int a, int b, int c, int d, int e, int f, int g, int h,
        char x, short y, int z, long long w, char v)
{ return a + h + x * 10 + y * 100 + z * 1000 + w * 10000 + v * 100000; }
double fl(double a, double b, double c, double d, double e, double f,
          double g, double h, float i, float j, double k, float l)
{ return a + h + i * 10 + j * 100 + k * 1000 + l * 10000; }
double hfa(double a, double b, double c, double d, double e, double f,
           double g, double h, float i, struct hf3 s, float j, struct hd t)
{ return a + h + i * 10 + s.x * 100 + s.z * 1000 + j * 10000 + t.y * 100000; }
long cs(int a, int b, int c, int d, int e, int f, int g, int h, char x,
        struct c3 s, char y, struct s12 t, short z)
{ return a + x * 10 + s.c * 100 + y * 1000 + t.c * 10000 + z * 100000; }
long al(int a, int b, int c, int d, int e, int f, int g, int h, char x,
        struct a16 s, char y)
{ return a + x * 10 + s.x * 100 + y * 1000; }
long i128(int a, __int128 v, int b) { return a + (long)v * 10 + b * 100; }
long n128(int a, struct n16 v, int b) { return a + (long)v.v * 10 + b * 100; }
int ext(signed char c, short s, unsigned char u, unsigned short w)
{ return c * 1000000 + s * 1000 + u + w; }
signed char rc(int x) { return (signed char)(x * 37); }
unsigned short rus(int x) { return (unsigned short)(x * 40000); }
long vs(int a, int b, int c, int d, int e, int f, int g, int h, char x,
        short y, ...)
{
    va_list ap; va_start(ap, y);
    long r = a + x * 10 + y * 100;
    r += va_arg(ap, int) * 1000;
    r += (long)(va_arg(ap, double) * 10000);
    r += va_arg(ap, long) * 100000;
    va_end(ap);
    return r;
}
long double ldf(long double a, double b, long double c) { return a * 10 + b * 100 + c * 1000; }
long vcopy(int n, ...)
{
    va_list ap, aq; long a = 0, b = 0;
    va_start(ap, n); va_copy(aq, ap);
    for (int i = 0; i < n; i++) a = a * 10 + va_arg(ap, long);
    for (int i = 0; i < n; i++) b = b * 10 + va_arg(aq, long);
    va_end(aq); va_end(ap);
    return a * 100000000 + b;
}
long vstruct(int n, ...)
{
    va_list ap; va_start(ap, n);
    struct c3 a = va_arg(ap, struct c3);
    struct al16 b = va_arg(ap, struct al16);
    struct s12 c = va_arg(ap, struct s12);
    struct hf2 d = va_arg(ap, struct hf2);
    struct hd e = va_arg(ap, struct hd);
    struct big24 f = va_arg(ap, struct big24);
    va_end(ap);
    return n + a.c * 10 + b.x * 100 + c.c * 1000 + (long)d.b * 10000 +
           (long)e.y * 100000 + f.c * 1000000;
}
E

cat > "$out/caller.c" << 'E'
#include "abi.h"
int printf(const char *, ...);
int snprintf(char *, unsigned long, const char *, ...);
int strcmp(const char *, const char *);
static int n, first;
static void check(int ok) { n++; if (!ok && !first) first = n; }
__attribute__((noinline)) static int id(int x) { return x; }
int main(void)
{
    struct c3 c3 = { 1, 2, 3 }; struct s12 s12 = { 4, 5, 6 };
    struct hf3 h3 = { 7, 8, 9 }; struct hd hd = { 1, 2 };
    struct a16 a16 = { 5 }; struct n16 n16 = { 6 };
    check(pk(1, 0, 0, 0, 0, 0, 0, 2, 3, 4, 5, 6, 7) == 3 + 30 + 400 + 5000 + 60000 + 700000);
    check(pk(1, 0, 0, 0, 0, 0, 0, 2, -3, -4, -5, -6, -7) == 3 - 30 - 400 - 5000 - 60000 - 700000);
    check(fl(1, 0, 0, 0, 0, 0, 0, 2, 3, 4, 5, 6) == 65433);
    check(hfa(1, 0, 0, 0, 0, 0, 0, 2, 3, h3, 4, hd) == 3 + 30 + 700 + 9000 + 40000 + 200000);
    check(cs(1, 0, 0, 0, 0, 0, 0, 0, 2, c3, 4, s12, 7) == 1 + 20 + 300 + 4000 + 60000 + 700000);
    check(al(1, 0, 0, 0, 0, 0, 0, 0, 2, a16, 3) == 1 + 20 + 500 + 3000);
    check(i128(1, 7, 2) == 271);
    check(n128(1, n16, 2) == 261);
    /* computed, so a caller that does not extend leaves junk above bit 8 */
    signed char c = (signed char)(id(100) + id(100));
    short s = (short)(id(30000) + id(30000));
    unsigned char u = (unsigned char)(id(200) + id(100));
    unsigned short w = (unsigned short)(id(60000) + id(10000));
    check(ext(c, s, u, w) == -56 * 1000000 + -5536 * 1000 + 44 + 4464);
    check(rc(id(5)) == -71);
    check(rus(id(3)) == 54464);
    int big = id(1000) * 1000;
    check(rc(id(5)) + big == -71 + 1000000);
    check(vs(1, 0, 0, 0, 0, 0, 0, 0, 2, 3, 4, 5.0, 6L) == 1 + 20 + 300 + 4000 + 50000 + 600000);
    check(ldf(1.5L, 2, 3.25L) == 15 + 200 + 3250);
    check(vcopy(8, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L) ==
          12345678L * 100000000 + 12345678L);
    {
        struct al16 al = { 5 }; struct hf2 h2 = { 1, 2 };
        struct big24 b24 = { 7, 8, 9 };
        check(vstruct(1, c3, al, s12, h2, hd, b24) ==
              1 + 30 + 500 + 6000 + 20000 + 200000 + 9000000);
    }
    check(sizeof(long double) == 8 && (char)-1 < 0);
    char buf[32];
    snprintf(buf, sizeof buf, "%.2Lf %d %hhd", ldf(1.5L, 2, 3.25L), (int)sizeof(__WCHAR_TYPE__) * ((__WCHAR_TYPE__)-1 < 0), (signed char)c);
    check(strcmp(buf, "3465.00 4 -56") == 0);
    if (first) printf("check %d of %d failed\n", first, n);
    return first ? first : 42;
}
E

fails=0
for opt in -O0 -O2; do
    rm -f "${out:?}"/*.o
    for f in callee caller; do
        "$EMBCC" --target=aarch64-apple-darwin $opt -c "$out/$f.c" \
            -o "$out/$f-e.o" 2> "$out/cc.log" || {
            echo "FAIL $opt: embcc could not compile $f.c:"; cat "$out/cc.log"
            exit 1; }
        cc -O1 -w -c "$out/$f.c" -o "$out/$f-c.o" || {
            echo "clang could not compile $f.c"; exit 1; }
    done
    for pair in "e e" "e c" "c e"; do
        set -- $pair
        cc -o "$out/p" "$out/caller-$1.o" "$out/callee-$2.o" \
            2> "$out/ln.log" || { echo "link failed:"; cat "$out/ln.log"
                                  exit 1; }
        msg=$("$out/p" 2>&1); rc=$?
        [ "$rc" = 42 ] && continue
        echo "FAIL $opt caller=$1 callee=$2 (e: EmbCC, c: clang): exit $rc $msg"
        fails=$((fails + 1))
    done
done
[ "$fails" = 0 ] || exit 1
echo "EmbCC and clang agree on Apple's arm64 convention and data model,"
echo "both directions, -O0 and -O2, run natively"

# C++ lays its own types out (src/cxx/type.c), and gave long double the
# 16 bytes it has on LP64 ELF -- here it is a double, so a class holding
# one was twice clang's size.
cat > "$out/ld.cpp" <<'EOF2'
struct S { char c; long double d; };
static_assert(sizeof(long double) == 8, "long double is a double here");
static_assert(sizeof(S) == 16, "and a class holding one is laid out so");
extern "C" int ldsz(void) { S s = { 1, 2.5L }; return (int)sizeof(s) + (int)(s.d * 2); }
EOF2
printf 'int ldsz(void);\nint main(void) { return ldsz() == 21 ? 0 : 1; }\n' \
    > "$out/ldm.c"
"$EMBCC" --target=aarch64-apple-darwin -c "$out/ld.cpp" -o "$out/ld.o" || {
    echo "FAIL: C++ does not lay long double out as Apple's 8-byte double"
    exit 1; }
cc -o "$out/ldm" "$out/ldm.c" "$out/ld.o" && "$out/ldm" || {
    echo "FAIL: a C++ class holding a long double disagrees with clang"; exit 1; }
echo "and C++ lays long double out as Apple's double"
