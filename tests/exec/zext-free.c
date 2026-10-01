/* A 32-bit value widened to 64 as unsigned, at -O2. On x86-64 every
 * 32-bit write of a register zeroes its upper half, so a temp written
 * once by 32-bit arithmetic already holds its zero extension and the
 * widening emits nothing: the CRC loop's `tab[(c ^ *p) & 0xff]` paid a
 * `mov %esi, %esi` a byte for it. Arithmetic that wraps past 32 bits
 * must come back truncated (add, sub, mul, neg, not, and/or/xor,
 * shifts by a constant and by a variable). A value that only got its
 * width by being COPIED must still be widened explicitly: a truncating
 * copy can leave the source's upper half in the register -- a local, a
 * temp, a parameter, a loop-carried value. Inputs come through volatile
 * so nothing folds; each widened value is returned, compared, used as
 * a table index, and summed in a loop. */
// expect-exit: 42
typedef unsigned long long u64;
static volatile u64 vbig = 0xA5A5A5A5FFFFFFF0ULL;   /* upper half set */
static volatile unsigned vu = 0xFFFFFFF0u, vk = 7;
static unsigned char tab[256];

__attribute__((noinline)) u64 w_add(unsigned a, unsigned b) { return (u64)(a + b); }
__attribute__((noinline)) u64 w_sub(unsigned a, unsigned b) { return (u64)(a - b); }
__attribute__((noinline)) u64 w_mul(unsigned a, unsigned b) { return (u64)(a * b); }
__attribute__((noinline)) u64 w_neg(unsigned a) { return (u64)(0u - a); }
__attribute__((noinline)) u64 w_not(unsigned a) { return (u64)~a; }
__attribute__((noinline)) u64 w_xor(unsigned a, unsigned b) { return (u64)(a ^ b); }
__attribute__((noinline)) u64 w_shl(unsigned a) { return (u64)(a << 4); }
__attribute__((noinline)) u64 w_shr(unsigned a) { return (u64)(a >> 4); }
__attribute__((noinline)) u64 w_shlv(unsigned a, unsigned k) { return (u64)(a << k); }
__attribute__((noinline)) u64 w_addk(unsigned a) { return (u64)(a + 0x20u); }
/* loads: an unsigned one is zero-filled; a signed one is widened from
 * its 32-bit value, never from the byte */
__attribute__((noinline)) u64 l_le32(const unsigned char *p)
{ return (u64)p[0] | (u64)p[1] << 8 | (u64)p[2] << 16 | (u64)p[3] << 24; }
__attribute__((noinline)) u64 l_u32(const unsigned *p, int i) { return (u64)p[i] + 1; }
__attribute__((noinline)) u64 l_s8(const signed char *p) { unsigned u = (unsigned)p[0]; return u; }
__attribute__((noinline)) u64 l_s32(const int *p) { unsigned u = (unsigned)p[0]; return u; }
/* width by copying: these must still be widened */
__attribute__((noinline)) u64 c_local(u64 v) { unsigned u = (unsigned)v; return u; }
__attribute__((noinline)) u64 c_param(unsigned u) { return u; }
__attribute__((noinline)) u64 c_cond(u64 v, int f) { unsigned u = f ? (unsigned)v : (unsigned)(v >> 8); return u; }
/* one name, two definitions: 32-bit arithmetic on one path, a
 * truncating copy on the other -- only the first is zero-extended */
__attribute__((noinline)) u64 c_mix(u64 v, int f) { unsigned u = f ? (unsigned)v + 1u : (unsigned)(v >> 8); return u; }
__attribute__((noinline)) u64 c_mix2(u64 v, int f) { unsigned u = (unsigned)(v >> 4); if (f) u = u * 3u; return (u64)u + 1; }
/* a 32-bit result that came back from a call: the ABI leaves the upper
 * half of rax undefined, and this callee's is not zero */
__attribute__((noinline)) unsigned r_low(u64 v) { return (unsigned)(v >> 8); }
__attribute__((noinline)) u64 c_call(u64 v) { unsigned u = r_low(v); return (u64)u * 2; }
/* carried round a loop: a truncating copy on the way in, 32-bit
 * arithmetic on the back edge -- the first trip's value is dirty */
__attribute__((noinline)) u64 c_carry(u64 v, int n)
{ u64 s = 0; unsigned u = (unsigned)v; for (int i = 0; i < n; i++) { s += (u64)u; u = u * 3u + 1u; } return s; }
__attribute__((noinline)) u64 c_loop(u64 v, int n)
{ u64 s = 0; unsigned u = (unsigned)v; for (int i = 0; i < n; i++) { s += u; u = (unsigned)(v >> i); } return s; }
/* the shape the change is for: a byte-table CRC-like walk */
__attribute__((noinline)) unsigned walk(const unsigned char *p, int n, unsigned c)
{ for (int i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xff] ^ (c >> 8); return c; }
__attribute__((noinline)) u64 sum_idx(const unsigned char *p, int n, unsigned x)
{ u64 s = 0; for (int i = 0; i < n; i++) { unsigned j = (x + (unsigned)i * 0x9E3779B9u) >> 24; s += tab[j] + (u64)j; } return s; }

static const unsigned char msg[] = "the quick brown fox jumps over the lazy dog";

int main(void)
{
    unsigned a = vu, k = vk;
    if (w_add(a, 0x20) != 0x10) return 1;
    if (w_sub(0x10, a) != 0x20) return 2;
    if (w_mul(a, 0x10) != 0xFFFFFF00ULL) return 3;
    if (w_neg(a) != 0x10) return 4;
    if (w_not(a) != 0xF) return 5;
    if (w_xor(a, 0xF0000000u) != 0x0FFFFFF0ULL) return 6;
    if (w_shl(a) != 0xFFFFFF00ULL) return 7;
    if (w_shr(a) != 0x0FFFFFFFULL) return 8;
    if (w_shlv(a, k) != 0xFFFFF800ULL) return 9;
    if (w_addk(a) != 0x10) return 10;
    static const unsigned char le[4] = { 0xF0, 0xDE, 0xBC, 0x9A };
    static const unsigned uw[2] = { 7, 0xFFFFFFFFu };
    static const signed char sc[1] = { -5 };
    static const int si[1] = { -9 };
    if (l_le32(le) != 0x9ABCDEF0ULL) return 21;
    if (l_u32(uw, (int)(k - 6)) != 0x100000000ULL) return 22;
    if (l_s8(sc) != 0xFFFFFFFBULL || l_s32(si) != 0xFFFFFFF7ULL) return 23;
    u64 big = vbig;
    if (c_local(big) != 0xFFFFFFF0ULL) return 11;
    if (c_param((unsigned)big) != 0xFFFFFFF0ULL) return 12;
    if (c_cond(big, 1) != 0xFFFFFFF0ULL || c_cond(big, 0) != 0xA5FFFFFFULL) return 13;
    if (c_mix(big, 1) != 0xFFFFFFF1ULL || c_mix(big, 0) != 0xA5FFFFFFULL) return 17;
    if (c_mix2(big, 0) != 0x5FFFFFFFULL + 1 || c_mix2(big, 1) != (u64)(0x5FFFFFFFu * 3u) + 1) return 18;
    if (c_call(big) != 0xA5FFFFFFULL * 2) return 19;
    {
        u64 w2 = 0; unsigned u = (unsigned)big;
        for (int i = 0; i < 5; i++) { w2 += u; u = u * 3u + 1u; }
        if (c_carry(big, 5) != w2) return 20;
    }
    /* u = low 32 of v >> i for i = 0..n-2, after the first term */
    u64 want = 0xFFFFFFF0ULL;
    for (int i = 0; i < 9; i++) want += (unsigned)(big >> i);
    if (c_loop(big, 10) != want) return 14;
    for (int i = 0; i < 256; i++) tab[i] = (unsigned char)(i * 7 + 3);
    /* reference walk through a volatile index, so it shares no code */
    unsigned c = 0xFFFFFFFFu;
    for (int i = 0; i < (int)sizeof msg - 1; i++) { volatile unsigned j = (c ^ msg[i]) & 0xff; c = tab[j] ^ (c >> 8); }
    if (walk(msg, (int)sizeof msg - 1, 0xFFFFFFFFu) != c) return 15;
    u64 s = 0;
    for (int i = 0; i < 40; i++) { volatile unsigned j = (0x12345678u + (unsigned)i * 0x9E3779B9u) >> 24; s += tab[j] + (u64)j; }
    if (sum_idx(msg, 40, 0x12345678u) != s) return 16;
    return 42;
}
