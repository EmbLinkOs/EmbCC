#!/bin/sh
# Varargs and floating aggregates ACROSS the compiler boundary: half the
# program built by EmbCC, half by the target's gcc, linked and run. Agreeing
# with ourselves proves nothing here — a va_list or an HFA in the wrong place
# does not crash, it reads garbage — so every check crosses the line:
#
#   * EmbCC calls a gcc-defined variadic function, and gcc calls an
#     EmbCC-defined one (mixed integer and floating arguments, past the
#     registers onto the stack);
#   * EmbCC hands its va_list to newlib's gcc-built vsnprintf, and gcc hands
#     ITS va_list to an EmbCC function that walks it with va_arg — the whole
#     claim that EmbCC's `char *` va_list is ABI-compatible with gcc's
#     (an array on SysV, a struct passed by reference on AAPCS64);
#   * Homogeneous Floating-point Aggregates of every shape AAPCS64 treats
#     specially — three floats, four doubles (exempt from pass-by-reference
#     at 32 bytes), an array member — in registers and spilled to the stack,
#     both directions and as return values. On x86-64 the same C checks SysV.
set -u
echo "TEST-MARKER cross-varargs"
. "$(dirname "$0")/../lib.sh"
out_dir="tests/golden/out/cross-varargs-$ARCH"
rm -rf "$out_dir"; mkdir -p "$out_dir"

cat > "$out_dir/x.h" << 'EOF'
#include <stdarg.h>
struct F3  { float a, b, c; };
struct D4  { double a, b, c, d; };
struct Arr { double v[2]; };
double gcc_vsum(int n, ...);          /* n pairs: (long, double) */
double emb_vsum(int n, ...);
int    gcc_call_emb_vsum(void);
int    emb_vfmt(char *buf, int cap, const char *f, va_list ap);
int    gcc_fmt_via_emb(char *buf, int cap, const char *f, ...);
double gcc_f3(struct F3 x);
double gcc_d4(struct D4 x);
double gcc_arr(struct Arr x);
double gcc_spill(struct D4 a, struct D4 b, struct D4 c, double tail);
struct D4 gcc_mk_d4(double base);
struct F3 gcc_mk_f3(float base);
double emb_d4(struct D4 x);
double emb_spill(struct D4 a, struct D4 b, struct D4 c, double tail);
int    gcc_call_emb_hfa(void);
EOF

cat > "$out_dir/gcchalf.c" << 'EOF'
#include <stdio.h>
#include "x.h"
double gcc_vsum(int n, ...)
{
    va_list ap; double s = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) { s += va_arg(ap, long); s += va_arg(ap, double); }
    va_end(ap);
    return s;
}
int gcc_call_emb_vsum(void)
{
    /* ten pairs: twenty arguments, well past both register files */
    double s = emb_vsum(10, 1L, 0.5, 2L, 0.25, 3L, 0.125, 4L, 1.0, 5L, 2.0,
                        6L, 3.0, 7L, 4.0, 8L, 5.0, 9L, 6.0, 10L, 7.0);
    return s == 55 + 28.875;
}
int gcc_fmt_via_emb(char *buf, int cap, const char *f, ...)
{
    va_list ap; va_start(ap, f);
    int n = emb_vfmt(buf, cap, f, ap);          /* gcc's va_list, into EmbCC */
    va_end(ap);
    return n;
}
double gcc_f3(struct F3 x)  { return x.a + 10 * x.b + 100 * x.c; }
double gcc_d4(struct D4 x)  { return x.a + 10 * x.b + 100 * x.c + 1000 * x.d; }
double gcc_arr(struct Arr x) { return x.v[0] - x.v[1]; }
double gcc_spill(struct D4 a, struct D4 b, struct D4 c, double tail)
{
    return gcc_d4(a) + gcc_d4(b) + gcc_d4(c) + tail;
}
struct D4 gcc_mk_d4(double base) { struct D4 r = { base, base + 1, base + 2, base + 3 }; return r; }
struct F3 gcc_mk_f3(float base)  { struct F3 r = { base, base * 2, base * 3 }; return r; }
int gcc_call_emb_hfa(void)
{
    struct D4 a = { 1, 2, 3, 4 }, b = { 5, 6, 7, 8 }, c = { 9, 10, 11, 12 };
    return emb_d4(a) == 4321 && emb_spill(a, b, c, 0.5) == 4321 + 8765 + 13209 + 0.5;
}
EOF

cat > "$out_dir/embhalf.c" << 'EOF'
#include <stdio.h>
#include <string.h>
#include "x.h"
double emb_vsum(int n, ...)
{
    va_list ap; double s = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) { s += va_arg(ap, long); s += va_arg(ap, double); }
    va_end(ap);
    return s;
}
int emb_vfmt(char *buf, int cap, const char *f, va_list ap)
{
    /* walk the first two by hand, then give the rest to newlib */
    int a = va_arg(ap, int);
    double d = va_arg(ap, double);
    int n = snprintf(buf, cap, "[%d %.2f]", a, d);
    return n + vsnprintf(buf + n, cap - n, f, ap);
}
static int emb_fmt(char *buf, int cap, const char *f, ...)
{
    va_list ap; va_start(ap, f);
    int n = vsnprintf(buf, cap, f, ap);         /* EmbCC's va_list, into newlib */
    va_end(ap);
    return n;
}
double emb_d4(struct D4 x) { return x.a + 10 * x.b + 100 * x.c + 1000 * x.d; }
double emb_spill(struct D4 a, struct D4 b, struct D4 c, double tail)
{
    return emb_d4(a) + emb_d4(b) + emb_d4(c) + tail;
}
int main(void)
{
    char buf[128];
    /* EmbCC -> gcc variadic, twenty arguments */
    if (gcc_vsum(10, 1L, 0.5, 2L, 0.25, 3L, 0.125, 4L, 1.0, 5L, 2.0,
                 6L, 3.0, 7L, 4.0, 8L, 5.0, 9L, 6.0, 10L, 7.0) != 55 + 28.875)
        return 1;
    /* gcc -> EmbCC variadic */
    if (!gcc_call_emb_vsum()) return 2;
    /* EmbCC's va_list into newlib's vsnprintf */
    emb_fmt(buf, sizeof buf, "%d|%s|%.3f|%ld|%c", -7, "mid", 2.5, 1234567890123L, 'Z');
    if (strcmp(buf, "-7|mid|2.500|1234567890123|Z") != 0) { puts(buf); return 3; }
    /* gcc's va_list into EmbCC (and on to newlib) */
    gcc_fmt_via_emb(buf, sizeof buf, "%s=%d", 3, 0.75, "x", 99);
    if (strcmp(buf, "[3 0.75]x=99") != 0) { puts(buf); return 4; }
    /* HFAs, EmbCC -> gcc */
    struct F3 f3 = { 1, 2, 3 };
    if (gcc_f3(f3) != 321) return 5;
    struct D4 d4 = { 1, 2, 3, 4 };
    if (gcc_d4(d4) != 4321) return 6;
    struct Arr ar = { { 7.5, 0.5 } };
    if (gcc_arr(ar) != 7) return 7;
    struct D4 b = { 5, 6, 7, 8 }, c = { 9, 10, 11, 12 };
    if (gcc_spill(d4, b, c, 0.5) != 4321 + 8765 + 13209 + 0.5) return 8;
    /* HFA returns from gcc */
    struct D4 r = gcc_mk_d4(2);
    if (r.a != 2 || r.b != 3 || r.c != 4 || r.d != 5) return 9;
    struct F3 rf = gcc_mk_f3(1.5f);
    if (rf.a != 1.5f || rf.b != 3.0f || rf.c != 4.5f) return 10;
    /* HFAs, gcc -> EmbCC */
    if (!gcc_call_emb_hfa()) return 11;
    puts("cross ok");
    return 42;
}
EOF

t_gcc_c "$out_dir/gcchalf.c" -std=c11 -I "$out_dir" -o "$out_dir/gcchalf.o" || {
    echo "gcc half failed to build"; exit 1; }
[ "$ARCH" = x86_64 ] && inc="-I $X86_NEWLIB/include" || inc="-I $AARCH64_NEWLIB/include"
# shellcheck disable=SC2086
"$EMBCC" --target="$TARGET" -c "$out_dir/embhalf.c" -o "$out_dir/embhalf.o" \
    -I "$out_dir" -I include $inc || { echo "embcc half failed to build"; exit 1; }
t_link "$out_dir/prog" "$out_dir/embhalf.o" "$out_dir/gcchalf.o" || {
    echo "link failed"; exit 1; }
out=$(t_run "$out_dir/prog"); got=$?
[ "$got" -eq 42 ] || {
    echo "cross run exited $got (a nonzero N is the Nth check); output: $out"
    exit 1; }
echo "EmbCC and gcc agree on varargs, va_list and HFAs across the boundary ($ARCH)"
