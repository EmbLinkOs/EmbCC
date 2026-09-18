#!/bin/sh
# long double ACROSS the compiler boundary: half the program built by EmbCC,
# half by the target's gcc, calling each other both ways. The format and the
# calling convention differ by target — x87 extended passed in MEMORY and
# returned in st0 on x86-64; IEEE binary128 passed and returned in q
# registers on aarch64, where a struct of long doubles is an HFA — and a
# mismatch does not crash, it computes with garbage. So every check crosses:
#
#   * fourteen arguments, long doubles among ints and doubles, past every
#     register file, both directions;
#   * variadic long doubles (va_arg on the other side), both directions;
#   * structs holding long doubles passed and returned by value;
#   * a quotient computed by one compiler compared with the other's —
#     the same bits (a checked-in exact rounding on each side).
set -u
echo "TEST-MARKER ldouble-abi"
. "$(dirname "$0")/../lib.sh"
out_dir="tests/golden/out/ldouble-abi-$ARCH"
rm -rf "$out_dir"; mkdir -p "$out_dir"

cat > "$out_dir/x.h" << 'EOF'
#include <stdarg.h>
struct L2 { long double a, b; };
struct LM { int tag; long double v; char c; };
#define MANY_ARGS int i0, long double a, double d0, long double b, long double c, \
    int i1, long double d, long double e, long double f, long double g,         \
    double d1, long double h, long double i, long double j
long double gcc_many(MANY_ARGS);
long double emb_many(MANY_ARGS);
long double gcc_vsum(int n, ...);          /* n values: long double, int, ... */
long double emb_vsum(int n, ...);
long double gcc_l2(struct L2 x);
long double emb_l2(struct L2 x);
struct L2 gcc_mk_l2(long double base);
struct L2 emb_mk_l2(long double base);
long double gcc_lm(struct LM x);
long double gcc_div(long double a, long double b);
long double emb_div(long double a, long double b);
int gcc_call_emb(void);
#if defined(__aarch64__)
struct L1 { long double v; };              /* an HFA of one: returned in q0 */
struct L1 gcc_mk_l1(long double v);
#endif
EOF

cat > "$out_dir/gcchalf.c" << 'EOF'
#include "x.h"
long double gcc_many(MANY_ARGS)
{
    return a + b + c + d + e + f + g + h + i + j + i0 + i1 + d0 + d1;
}
long double gcc_vsum(int n, ...)
{
    va_list ap; long double s = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++)
        s += (k % 2) ? va_arg(ap, int) : va_arg(ap, long double);
    va_end(ap);
    return s;
}
long double gcc_l2(struct L2 x) { return x.a - x.b; }
struct L2 gcc_mk_l2(long double base) { struct L2 r = { base, base / 3 }; return r; }
long double gcc_lm(struct LM x) { return x.v * x.tag + x.c; }
long double gcc_div(long double a, long double b) { return a / b; }
#if defined(__aarch64__)
struct L1 gcc_mk_l1(long double v) { struct L1 r = { v * 2 }; return r; }
#endif
int gcc_call_emb(void)
{
    if (emb_many(1, 1, 0.5, 2, 3, 2, 4, 5, 6, 7, 0.25, 8, 9, 10) != 58.75L)
        return 0;
    if (emb_vsum(6, 1.5L, 2, 2.5L, 3, 0.25L, 4) != 13.25L) return 0;
    struct L2 x = { 10, 0.5L };
    if (emb_l2(x) != 9.5L) return 0;
    struct L2 r = emb_mk_l2(9);
    if (r.a != 9 || r.b != 9.0L / 3) return 0;
    if (emb_div(1, 3) != 1.0L / 3) return 0;        /* EmbCC's bits == gcc's */
    return 1;
}
EOF

cat > "$out_dir/embhalf.c" << 'EOF'
#include "x.h"
long double emb_many(MANY_ARGS)
{
    return a + b + c + d + e + f + g + h + i + j + i0 + i1 + d0 + d1;
}
long double emb_vsum(int n, ...)
{
    va_list ap; long double s = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++)
        s += (k % 2) ? va_arg(ap, int) : va_arg(ap, long double);
    va_end(ap);
    return s;
}
long double emb_l2(struct L2 x) { return x.a - x.b; }
struct L2 emb_mk_l2(long double base) { struct L2 r = { base, base / 3 }; return r; }
long double emb_div(long double a, long double b) { return a / b; }
int main(void)
{
    /* EmbCC -> gcc */
    if (gcc_many(1, 1, 0.5, 2, 3, 2, 4, 5, 6, 7, 0.25, 8, 9, 10) != 58.75L)
        return 1;
    if (gcc_vsum(6, 1.5L, 2, 2.5L, 3, 0.25L, 4) != 13.25L) return 2;
    struct L2 x = { 10, 0.5L };
    if (gcc_l2(x) != 9.5L) return 3;
    struct L2 r = gcc_mk_l2(9);
    if (r.a != 9 || r.b != 9.0L / 3) return 4;
    struct LM m = { 3, 0.5L, 2 };
    if (gcc_lm(m) != 3.5L) return 5;
    volatile long double one = 1, three = 3;
    if (gcc_div(one, three) != one / three) return 6;  /* gcc's bits == EmbCC's */
#if defined(__aarch64__)
    struct L1 l1 = gcc_mk_l1(21);
    if (l1.v != 42) return 7;
#endif
    /* gcc -> EmbCC */
    if (!gcc_call_emb()) return 8;
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
t_run "$out_dir/prog" >/dev/null; got=$?
[ "$got" -eq 42 ] || {
    echo "cross run exited $got (a nonzero N is the Nth check)"; exit 1; }
echo "EmbCC and gcc agree on long double across the boundary ($ARCH): arguments"
echo "past the registers, varargs, structs by value, returns, the same bits"
