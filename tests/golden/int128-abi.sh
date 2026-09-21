#!/bin/sh
# __int128 ACROSS the compiler boundary: half the program built by EmbCC,
# half by the target's gcc, calling each other both ways. Both ABIs pass one
# in two integer registers and return it in two (rax:rdx, x0:x1); on aarch64
# the pair starts at an even register (AAPCS64 C.8: after an int in x0 it
# takes x2:x3, leaving x1), and once the registers run out it is 16 bytes
# of stack, 16-aligned, on both. So every check crosses:
#
#   * arguments past the registers, 128-bit ones among ints and longs;
#   * a struct holding one, by value, and one returned;
#   * values computed by one compiler (a product, a quotient, shifts) and
#     compared with the other's;
#   * variadic calls: one compiler's va_arg reading the other's arguments
#     (in registers, then — x86-64 with one register left, aarch64 past an
#     odd one — in 16-aligned stack slots);
#   * a packed struct of bit-fields — one of them 128 bits at bit 3, over
#     17 bytes — filled by one compiler and read by the other.
set -u
echo "TEST-MARKER int128-abi"
. "$(dirname "$0")/../lib.sh"
out_dir="tests/golden/out/int128-abi-$ARCH"
rm -rf "$out_dir"; mkdir -p "$out_dir"

cat > "$out_dir/x.h" << 'EOF'
typedef unsigned __int128 u128;
typedef __int128 i128;
struct Q { signed char c; i128 v; };
#define MANY_ARGS int k0, u128 a, long l0, u128 b, int k1, i128 c, u128 d, \
    u128 e, int k2, i128 f
u128 gcc_many(MANY_ARGS);
u128 emb_many(MANY_ARGS);
i128 gcc_q(struct Q q);
struct Q gcc_mk_q(i128 v);
i128 emb_q(struct Q q);
struct Q emb_mk_q(i128 v);
u128 gcc_mul(u128 a, u128 b);
u128 emb_mul(u128 a, u128 b);
i128 gcc_div(i128 a, i128 b);
i128 emb_div(i128 a, i128 b);
struct __attribute__((packed)) PQ {
    unsigned char c : 3; i128 x : 128; unsigned char d : 5;
    unsigned long long y : 64; signed char e : 7; u128 z : 90;
};
void gcc_fill_pq(struct PQ *p);
int gcc_check_pq(const struct PQ *p);
i128 gcc_va(int n, ...);
i128 emb_va(int n, ...);
int gcc_call_emb(void);
EOF

cat > "$out_dir/gcchalf.c" << 'EOF'
#include <stdarg.h>
#include "x.h"
void gcc_fill_pq(struct PQ *p)
{
    p->c = 5; p->x = -((i128)1 << 120) + 77; p->d = 19;
    p->y = 0x8765432187654321ULL; p->e = -33; p->z = ((u128)1 << 89) | 12345;
}
int gcc_check_pq(const struct PQ *p)
{
    return p->c == 2 && p->x == ((i128)1 << 126) - 99 && p->d == 31 &&
           p->y == 0x0123456789abcdefULL && p->e == 63 &&
           p->z == (((u128)1 << 90) - 1);
}
i128 gcc_va(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    i128 s = 0;
    for (int k = 0; k < n; k++) {
        int w = va_arg(ap, int);
        s += va_arg(ap, i128) * w;
    }
    va_end(ap);
    return s;
}
u128 gcc_many(MANY_ARGS)
{
    return a + b + (u128)c + d + e + (u128)f + k0 + k1 + k2 + l0;
}
i128 gcc_q(struct Q q) { return q.v * q.c; }
struct Q gcc_mk_q(i128 v) { struct Q r = { 3, v << 70 }; return r; }
u128 gcc_mul(u128 a, u128 b) { return a * b; }
i128 gcc_div(i128 a, i128 b) { return a / b; }
static const u128 big = ((u128)0x0123456789abcdefUL << 64) | 0xfedcba9876543210UL;
int gcc_call_emb(void)
{
    if (emb_many(1, big, 2, big >> 3, 3, -(i128)5, 7, big << 5, 4, -1) !=
        big + (big >> 3) + (u128)-(i128)5 + 7 + (big << 5) + (u128)(i128)-1 +
        1 + 3 + 4 + 2)
        return 0;
    struct Q q = { -2, (i128)big };
    if (emb_q(q) != (i128)big * -2) return 0;
    struct Q r = emb_mk_q(-9);
    if (r.c != 5 || r.v != (i128)-9 * 1000000007) return 0;
    if (emb_mul(big, big >> 7) != big * (big >> 7)) return 0;
    if (emb_div(-(i128)big, 1000000007) != -(i128)big / 1000000007) return 0;
    if (emb_va(4, 1, (i128)big, 2, (i128)-3, 3, (i128)big >> 9, 4, (i128)7) !=
        (i128)big - 6 + ((i128)big >> 9) * 3 + 28) return 0;
    return 1;
}
EOF

cat > "$out_dir/embhalf.c" << 'EOF'
#include <stdarg.h>
#include "x.h"
i128 emb_va(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    i128 s = 0;
    for (int k = 0; k < n; k++) {
        int w = va_arg(ap, int);
        s += va_arg(ap, i128) * w;
    }
    va_end(ap);
    return s;
}
u128 emb_many(MANY_ARGS)
{
    return a + b + (u128)c + d + e + (u128)f + k0 + k1 + k2 + l0;
}
i128 emb_q(struct Q q) { return q.v * q.c; }
struct Q emb_mk_q(i128 v) { struct Q r = { 5, v * 1000000007 }; return r; }
u128 emb_mul(u128 a, u128 b) { return a * b; }
i128 emb_div(i128 a, i128 b) { return a / b; }
static const u128 big = ((u128)0x0123456789abcdefUL << 64) | 0xfedcba9876543210UL;
int main(void)
{
    /* EmbCC -> gcc */
    if (gcc_many(1, big, 2, big >> 3, 3, -(i128)5, 7, big << 5, 4, -1) !=
        big + (big >> 3) + (u128)-(i128)5 + 7 + (big << 5) + (u128)(i128)-1 +
        1 + 3 + 4 + 2)
        return 1;
    struct Q q = { 4, -(i128)big };
    if (gcc_q(q) != -(i128)big * 4) return 2;
    struct Q r = gcc_mk_q(-3);
    if (r.c != 3 || r.v != (i128)-3 << 70) return 3;
    if (gcc_mul(big, big >> 9) != big * (big >> 9)) return 4;  /* same bits */
    if (gcc_div((i128)big, -77) != (i128)big / -77) return 5;
    if (gcc_va(5, 1, (i128)1, 2, (i128)big, 3, (i128)-big, 4, (i128)5, 5,
               (i128)big << 3) != 1 + 2 * (i128)big - 3 * (i128)big + 20 +
                                  5 * ((i128)big << 3)) return 7;
    /* gcc -> EmbCC */
    if (!gcc_call_emb()) return 6;
    /* a packed struct each way */
    struct PQ a, b;
    __builtin_memset(&a, 0, sizeof a);
    gcc_fill_pq(&a);
    if (sizeof a != 38 || a.c != 5 || a.x != -((i128)1 << 120) + 77 ||
        a.d != 19 || a.y != 0x8765432187654321ULL || a.e != -33 ||
        a.z != (((u128)1 << 89) | 12345))
        return 8;
    __builtin_memset(&b, 0, sizeof b);
    b.c = 2; b.x = ((i128)1 << 126) - 99; b.d = 31;
    b.y = 0x0123456789abcdefULL; b.e = 63; b.z = ((u128)1 << 90) - 1;
    if (!gcc_check_pq(&b))
        return 9;
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
echo "EmbCC and gcc agree on __int128 across the boundary ($ARCH): arguments"
echo "past the registers, structs by value, returns, the same arithmetic,"
echo "va_arg of the other's variadic arguments, packed bit-fields of it"
