#!/bin/sh
# Struct and float arguments at the edges of the calling convention,
# half the program from gcc and half from EmbCC, in both directions, at
# -O0 and -O2. sysv-abi.sh covers the common shapes; these are the ones
# an ABI fuzzer and gcc found EmbCC getting wrong:
#
#   * a struct that finds too few registers left goes WHOLE on the stack
#     and takes none (the x86-64 callee read a seventh struct{short}
#     from r9) -- including a 3-byte one and a 16-aligned one;
#   * floats past the last FP argument register, whose home the -O2
#     allocator made a register (x86-64 refused the function; aarch64
#     stored the ninth double a gigabyte above sp);
#   * AAPCS64 C.10: a composite of natural alignment 16 starts at an
#     even x register -- natural meaning its members', so aligned(16)
#     on the struct alone does not count, and packing does;
#   * System V: an eightbyte that is only padding takes no register, an
#     __int128 member fills both eightbytes, and a packed struct with an
#     unaligned field is MEMORY.
set -u
echo "TEST-MARKER struct-abi-edges"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/struct-abi-edges-$ARCH
rm -rf "$out"; mkdir -p "$out"

cat > "$out/abi.h" << 'E'
struct c3 { char a, b, c; };
struct s2 { short m; };
struct a16 { __int128 v; };
struct l2 { long a, b; };
struct mixd { double d; long l; };
struct __attribute__((aligned(16))) ov { long x; };
struct __attribute__((aligned(16))) od { double x; };
struct m1 { long x __attribute__((aligned(16))); };
struct m4 { struct { __int128 v; } in; };
struct __attribute__((packed)) m5 { __int128 v; };
struct m7 { struct ov in; };
struct __attribute__((packed, aligned(16))) m8 { __int128 v; };
#pragma pack(4)
struct m9 { __int128 v; };
#pragma pack()
struct __attribute__((aligned(16))) hf { double a, b; };
struct __attribute__((packed)) p1 { char c; long l; };
struct __attribute__((packed)) p2 { char c; int i; };
struct __attribute__((packed)) p3 { int i; char c; };
struct __attribute__((packed)) p4 { short s; int i; };
struct __attribute__((packed)) p5 { char c; double d; };
struct cz { _Complex float z; };
struct cz2 { float f; _Complex float z; };
struct ub { long :64; long y; };
long st3(long a, long b, long c, long d, long e, long f, struct c3 s);
long st2(long a, long b, long c, long d, long e, long f, struct s2 s, struct l2 t);
long fa16(long a, long b, long c, long d, long e, struct a16 t, long z);
long fodd(long a, long b, long c, long d, long e, long f, long g, struct a16 t);
long fl2(long a, long b, long c, long d, long e, struct l2 t, long z);
double fmix(double a, double b, double c, double d, double e, double f,
            double g, struct mixd m, double h, double i);
double d10(double a, double b, double c, double d, double e, double f,
           double g, double h, double i, double j);
float f10(float a, float b, float c, float d, float e, float f, float g,
          float h, float i, float j);
long g1(long a, struct ov s, long z);
long h1(long a, struct m1 s, long z);
long h4(long a, struct m4 s, long z);
long h5(long a, struct m5 s, long z);
long k7(long a, struct m7 s, long z);
long k8(long a, struct m8 s, long z);
long k9(long a, struct m9 s, long z);
long ks(long a, long b, long c, long d, long e, long f, long g, long h,
        long i, struct ov s, long z);
long ks9(long a, long b, long c, long d, long e, long f, long g, long h,
         long i, struct m9 s, long z);
long ksi(long a, long b, long c, long d, long e, long f, long g, long h,
         long i, struct m7 s, long z);
double hfs(double a, double b, double c, double d, double e, double f,
           double g, long i, struct hf s, double z);
long q1(long a, struct p1 s, long z);
long q2(long a, struct p2 s, long z);
long q3(long a, struct p3 s, long z);
long q4(long a, struct p4 s, long z);
double q5(long a, struct p5 s, double z);
double q6(double a, struct cz s, double z);
double q7(double a, struct cz2 s, double z);
long q8(long a, struct ub s, long z);
struct ov rov(long x);
struct od rod(double x);
struct p1 rp1(long l);
struct cz2 rcz2(float f);
E

cat > "$out/callee.c" << 'E'
#include "abi.h"
long st3(long a, long b, long c, long d, long e, long f, struct c3 s)
{ return a + b + c + d + e + f + s.a + s.b * 10 + s.c * 100; }
long st2(long a, long b, long c, long d, long e, long f, struct s2 s, struct l2 t)
{ return a + b + c + d + e + f + s.m * 100 + t.a * 1000 + t.b * 10000; }
long fa16(long a, long b, long c, long d, long e, struct a16 t, long z)
{ return a + b + c + d + e + (long)t.v * 10 + z * 1000; }
long fodd(long a, long b, long c, long d, long e, long f, long g, struct a16 t)
{ return a + b + c + d + e + f + g * 100 + (long)(t.v >> 64) * 1000; }
long fl2(long a, long b, long c, long d, long e, struct l2 t, long z)
{ return a + b + c + d + e + t.a * 10 + t.b * 100 + z * 1000; }
double fmix(double a, double b, double c, double d, double e, double f,
            double g, struct mixd m, double h, double i)
{ return a + b + c + d + e + f + g + m.d * 10 + m.l * 100 + h * 1000 + i * 10000; }
double d10(double a, double b, double c, double d, double e, double f,
           double g, double h, double i, double j)
{ return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10; }
float f10(float a, float b, float c, float d, float e, float f, float g,
          float h, float i, float j)
{ return j - i + h - g + f - e + d - c + b - a; }
long g1(long a, struct ov s, long z) { return a + s.x * 10 + z * 100; }
long h1(long a, struct m1 s, long z) { return a + s.x * 10 + z * 100; }
long h4(long a, struct m4 s, long z) { return a + (long)s.in.v * 10 + z * 100; }
long h5(long a, struct m5 s, long z) { return a + (long)s.v * 10 + z * 100; }
long k7(long a, struct m7 s, long z) { return a + s.in.x * 10 + z * 100; }
long k8(long a, struct m8 s, long z) { return a + (long)s.v * 10 + z * 100; }
long k9(long a, struct m9 s, long z) { return a + (long)s.v * 10 + z * 100; }
long ks(long a, long b, long c, long d, long e, long f, long g, long h,
        long i, struct ov s, long z)
{ return a + i + s.x * 10 + z * 100; }
long ks9(long a, long b, long c, long d, long e, long f, long g, long h,
         long i, struct m9 s, long z)
{ return a + i + (long)s.v * 10 + z * 100; }
long ksi(long a, long b, long c, long d, long e, long f, long g, long h,
         long i, struct m7 s, long z)
{ return a + i + s.in.x * 10 + z * 100; }
double hfs(double a, double b, double c, double d, double e, double f,
           double g, long i, struct hf s, double z)
{ return a + i + s.a * 10 + s.b * 100 + z * 1000; }
long q1(long a, struct p1 s, long z) { return a + s.c * 10 + s.l * 100 + z * 1000; }
long q2(long a, struct p2 s, long z) { return a + s.c * 10 + s.i * 100 + z * 1000; }
long q3(long a, struct p3 s, long z) { return a + s.i * 10 + s.c * 100 + z * 1000; }
long q4(long a, struct p4 s, long z) { return a + s.s * 10 + s.i * 100 + z * 1000; }
double q5(long a, struct p5 s, double z) { return a + s.c * 10 + s.d * 100 + z * 1000; }
double q6(double a, struct cz s, double z)
{ return a + __real__ s.z * 10 + __imag__ s.z * 100 + z * 1000; }
double q7(double a, struct cz2 s, double z)
{ return a + s.f * 10 + __real__ s.z * 100 + __imag__ s.z * 1000 + z * 10000; }
long q8(long a, struct ub s, long z) { return a + s.y * 10 + z * 100; }
struct ov rov(long x) { struct ov r = { x }; return r; }
struct od rod(double x) { struct od r = { x }; return r; }
struct p1 rp1(long l) { struct p1 r = { 1, l }; return r; }
struct cz2 rcz2(float f) { struct cz2 r; r.f = f; __real__ r.z = f; __imag__ r.z = 2 * f; return r; }
E

cat > "$out/caller.c" << 'E'
#include "abi.h"
int printf(const char *, ...);
static int n, first;
static void check(int ok) { n++; if (!ok && !first) first = n; }
int main(void)
{
    struct c3 c3 = { 1, 2, 3 }; struct s2 s2 = { 7 }; struct l2 l2 = { 4, 5 };
    struct a16 a7 = { 7 }; struct a16 a9 = { (__int128)9 << 64 };
    struct mixd md = { 2, 3 };
    struct ov ov = { 3 }; struct m1 m1 = { 3 }; struct m4 m4 = { { 3 } };
    struct m5 m5 = { 3 }; struct m7 m7 = { { 3 } }; struct m8 m8 = { 3 };
    struct m9 m9 = { 3 }; struct hf hf = { 3, 4 };
    struct p1 p1 = { 2, 3 }; struct p2 p2 = { 2, 3 }; struct p3 p3 = { 2, 3 };
    struct p4 p4 = { 2, 3 }; struct p5 p5 = { 2, 3 };
    struct cz cz; __real__ cz.z = 2; __imag__ cz.z = 3;
    struct cz2 cz2; cz2.f = 2; __real__ cz2.z = 3; __imag__ cz2.z = 4;
    struct ub ub; ub.y = 2;
    check(st3(1, 2, 3, 4, 5, 6, c3) == 21 + 321);
    check(st2(1, 2, 3, 4, 5, 6, s2, l2) == 21 + 700 + 54000);
    check(fa16(1, 2, 3, 4, 5, a7, 6) == 15 + 70 + 6000);
    check(fodd(1, 2, 3, 4, 5, 6, 7, a9) == 21 + 700 + 9000);
    check(fl2(1, 2, 3, 4, 5, l2, 6) == 15 + 540 + 6000);
    check(fmix(1, 2, 3, 4, 5, 6, 7, md, 8, 9) == 28 + 320 + 98000);
    check(d10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 385);
    check(f10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 5);
    check(g1(1, ov, 2) == 231);
    check(h1(1, m1, 2) == 231);
    check(h4(1, m4, 2) == 231);
    check(h5(1, m5, 2) == 231);
    check(k7(1, m7, 2) == 231);
    check(k8(1, m8, 2) == 231);
    check(k9(1, m9, 2) == 231);
    check(ks(1, 0, 0, 0, 0, 0, 0, 0, 5, ov, 2) == 236);
    check(ks9(1, 0, 0, 0, 0, 0, 0, 0, 5, m9, 2) == 236);
    check(ksi(1, 0, 0, 0, 0, 0, 0, 0, 5, m7, 2) == 236);
    check(hfs(1, 0, 0, 0, 0, 0, 0, 5, hf, 2) == 2436);
    check(q1(1, p1, 4) == 4321);
    check(q2(1, p2, 4) == 4321);
    check(q3(1, p3, 4) == 4321);
    check(q4(1, p4, 4) == 4321);
    check(q5(1, p5, 4) == 4321);
    check(q6(1, cz, 4) == 4321);
    check(q7(1, cz2, 5) == 54321);
    check(q8(1, ub, 3) == 321);
    check(rov(7).x == 7);
    check(rod(7).x == 7);
    check(rp1(7).l == 7 && rp1(7).c == 1);
    struct cz2 r = rcz2(3);
    check(r.f == 3 && __real__ r.z == 3 && __imag__ r.z == 6);
    if (first)
        printf("check %d failed\n", first);
    return first ? first : 42;
}
E

fails=0
for opt in -O0 -O2; do
    for f in callee caller; do
        "$EMBCC" --target="$TARGET" $opt -c "$out/$f.c" -o "$out/$f-e$opt.o" \
            -I "$out" || { echo "embcc $opt cannot compile $f.c"; exit 1; }
        t_gcc_c "$out/$f.c" -std=gnu11 -O1 -w -Wno-psabi -o "$out/$f-g$opt.o" \
            -I "$out" || { echo "gcc cannot compile $f.c"; exit 1; }
    done
    for pair in "e g" "g e"; do
        set -- $pair
        t_link "$out/p$1$2$opt" "$out/caller-$1$opt.o" "$out/callee-$2$opt.o" \
            > "$out/ln.log" 2>&1 || { echo "link failed:"; cat "$out/ln.log"
                                     exit 1; }
        msg=$(t_run "$out/p$1$2$opt"); rc=$?
        [ "$rc" = 42 ] && continue
        who=$([ "$1" = e ] && echo "EmbCC calling gcc" ||
              echo "gcc calling EmbCC")
        echo "FAIL $opt $who: exit $rc ($msg -- the Nth check in caller.c)"
        fails=$((fails + 1))
    done
done
[ "$fails" = 0 ] || exit 1
echo "EmbCC and gcc agree on $ARCH's edge cases of struct and float"
echo "passing, both directions, -O0 and -O2"
