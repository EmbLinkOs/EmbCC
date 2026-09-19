/* Packed bit-fields across their storage unit wider than 8 bytes: a
 * 64-bit field at bit 7 of its first byte spans 9 (only 8 were read and
 * written before), an __int128 field 16 or 17 — read, written, compound-
 * assigned, statically initialized, neighbours kept. gcc agrees on both
 * targets (tests/golden/int128-abi.sh also crosses the compilers). */
// expect-exit: 42
typedef unsigned __int128 u128;
typedef __int128 i128;
#define W(hi, lo) ((u128)(hi) << 64 | (u128)(lo))
struct __attribute__((packed)) P { unsigned char c : 7; unsigned long long x : 64; unsigned char d : 1; };
struct __attribute__((packed)) Q { unsigned char c : 3; i128 x : 128; unsigned char d : 5; };   /* 17 bytes */
struct __attribute__((packed)) R { unsigned char c : 4; u128 x : 100; signed char d : 4; long long y : 60; };
static struct P sp = { 5, 0xfedcba9876543210ULL, 1 };
static struct Q sq = { 6, -((i128)1 << 126) + 12345, 17 };
static struct R sr = { 9, W(0xfffffffffUL, 0x123456789abcdef0UL), -3, -7 };
int main(void)
{
    struct P p = { 3, 0x8123456789abcdefULL, 1 };
    if (sizeof(struct P) != 9 || sizeof(struct Q) != 17 || sizeof(struct R) != 21) return 1;
    if (p.c != 3 || p.x != 0x8123456789abcdefULL || p.d != 1) return 2;
    p.x += 0x1111;
    if (p.x != 0x8123456789abdf00ULL || p.c != 3 || p.d != 1) return 3;
    if (sp.c != 5 || sp.x != 0xfedcba9876543210ULL || sp.d != 1) return 4;
    if (sq.c != 6 || sq.x != -((i128)1 << 126) + 12345 || sq.d != 17) return 5;
    struct Q q = sq;
    q.x = ~q.x;
    if (q.c != 6 || q.x != ((i128)1 << 126) - 12346 || q.d != 17) return 6;
    q.d = 30; q.c = 1;
    if (q.x != ((i128)1 << 126) - 12346) return 7;
    if (sr.c != 9 || sr.x != W(0xfffffffffUL, 0x123456789abcdef0UL) || sr.d != -3 || sr.y != -7) return 8;
    struct R r = sr;
    r.x *= 3; r.y <<= 3; r.d = 7;
    if (r.x != (W(0xfffffffffUL, 0x123456789abcdef0UL) * 3 & W(0xfffffffffUL, ~0UL)) || r.y != -56 || r.c != 9 || r.d != 7) return 9;
    unsigned char *b = (unsigned char *)&q;
    unsigned sum = 0;
    for (int k = 0; k < 17; k++) sum = sum * 31 + b[k];
    return sum == 0 ? 10 : 42;
}
