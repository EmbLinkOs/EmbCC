// expect-exit: 42
/* The 64-bit product of two 32-bit values: `(int64_t)a * b`, which a
 * 32-bit machine with a widening multiply does in one instruction
 * (smull/umull, mul + mulh/mulhu, mult/multu...) and must do with the
 * operands' own signedness -- a signed product of INT32_MIN by itself is
 * 2^62, an unsigned one of 0x80000000 by itself is 2^62 too, but
 * 0xffffffff squared is 2^64 - 2^33 + 1 one way and 1 the other.
 *
 * The reference multiplies 64-bit values read from volatile objects,
 * which no compiler can see to be extensions of anything: a full 64 by
 * 64 multiply. Constant operands, mixed signedness (left as the 64-bit
 * multiply), an accumulating sum (smlal/umlal on Arm), and a narrow
 * read of a 64-bit value as one operand are all here. */
#include <stdint.h>

typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;

static volatile i64 vsa, vsb;
static volatile u64 vua, vub;
static int bad;

static i64 smul(i32 a, i32 b) { return (i64)a * b; }
static u64 umul(u32 a, u32 b) { return (u64)a * b; }
static i64 mixed(i32 a, u32 b) { return (i64)a * (i64)b; }
static i64 sk1000(i32 a) { return (i64)a * 1000; }
static i64 skneg(i32 a) { return (i64)a * -7; }
static i64 skmax(i32 a) { return (i64)a * 0x7fffffff; }
static i64 skmin(i32 a) { return (i64)a * (i64)INT32_MIN; }
static u64 uk3e9(u32 a) { return (u64)a * 3000000000u; }
static u64 ukff(u32 a) { return (u64)a * 0xffffffffu; }
/* constants that only ONE extension makes: a signed operand by one that
 * needs 33 bits signed, an unsigned one by a negative 64-bit value --
 * each stays a 64-bit multiply */
static i64 skbig(i32 a) { return (i64)a * 3000000000LL; }
static i64 skffff(i32 a) { return (i64)a * 0xffffffffLL; }
static u64 ukneg(u32 a) { return (u64)a * (u64)-7; }
static i64 narrow(i64 a, i64 b) { return (i64)(i32)a * (i32)b; }
static i64 sacc(const i32 *p, const i32 *q, int n, i64 s)
{
    for (int i = 0; i < n; i++)
        s += (i64)p[i] * q[i];
    return s;
}
static u64 uacc(const u32 *p, const u32 *q, int n, u64 s)
{
    for (int i = 0; i < n; i++)
        s += (u64)p[i] * q[i];
    return s;
}
static i64 sacc_rev(i32 a, i32 b, i64 s) { return (i64)a * b + s; }

static i64 sref(i32 a, i32 b) { vsa = a; vsb = b; return vsa * vsb; }
static u64 uref(u32 a, u32 b) { vua = a; vub = b; return vua * vub; }

static const u32 vals[] = {
    0, 1, 2, 3, 7, 1000, 46340, 46341, 65535, 65536, 65537, 0x7ffffffeu,
    0x7fffffffu, 0x80000000u, 0x80000001u, 0xfffffffeu, 0xffffffffu,
    0xfffffff9u, 0xfffffc18u, 0xffff0000u, 0x0000ffffu, 0x12345678u,
    0x9abcdef0u, 0xdeadbeefu, 3000000000u
};
#define NV (int)(sizeof vals / sizeof vals[0])

static void pair(u32 x, u32 y)
{
    i32 a = (i32)x, b = (i32)y;
    if (smul(a, b) != sref(a, b)) bad++;
    if (umul(x, y) != uref(x, y)) bad++;
    vsa = a; vsb = (i64)y;
    if (mixed(a, y) != vsa * vsb) bad++;
    if (sk1000(a) != sref(a, 1000)) bad++;
    if (skneg(a) != sref(a, -7)) bad++;
    if (skmax(a) != sref(a, 0x7fffffff)) bad++;
    if (skmin(a) != sref(a, INT32_MIN)) bad++;
    if (uk3e9(x) != uref(x, 3000000000u)) bad++;
    if (ukff(x) != uref(x, 0xffffffffu)) bad++;
    vsa = a; vsb = 3000000000LL;
    if (skbig(a) != vsa * vsb) bad++;
    vsa = a; vsb = 0xffffffffLL;
    if (skffff(a) != vsa * vsb) bad++;
    vua = x; vub = (u64)-7;
    if (ukneg(x) != vua * vub) bad++;
    /* the high halves of the 64-bit arguments must not be read */
    if (narrow((i64)0x5a5a5a5a00000000LL + x, (i64)0x1234567800000000LL + y) !=
        sref(a, b)) bad++;
    if (sacc_rev(a, b, (i64)x << 20) != sref(a, b) + ((i64)x << 20)) bad++;
}

int main(void)
{
    static i32 ps[64], qs[64];
    static u32 pu[64], qu[64];
    i64 se = -12345;
    u64 ue = 987654321u;
    u32 seed = 88172645u;
    /* the edges, every pair: INT_MIN*INT_MIN, UINT_MAX*UINT_MAX, mixed */
    for (int i = 0; i < NV; i++)
        for (int j = 0; j < NV; j++)
            pair(vals[i], vals[j]);
    for (int i = 0; i < 2000; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        u32 x = seed;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        pair(x, seed >> (i & 31));
    }
    for (int i = 0; i < 64; i++) {
        u32 x = vals[i % NV] ^ (u32)(i * 2654435761u);
        u32 y = vals[(i * 7) % NV] + (u32)i;
        /* small enough that the signed sum cannot overflow */
        ps[i] = (i32)x >> 8; qs[i] = (i32)y >> 8; pu[i] = x; qu[i] = y;
        se += sref(ps[i], qs[i]);
        ue += uref(pu[i], qu[i]);
    }
    if (sacc(ps, qs, 64, -12345) != se) bad++;
    if (uacc(pu, qu, 64, 987654321u) != ue) bad++;
    return bad == 0 ? 42 : 1;
}
