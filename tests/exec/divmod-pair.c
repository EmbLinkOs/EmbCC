/* `q = a / b; r = a % b;` divides once: the remainder is a - q * b, which
 * C's truncating division makes exact. Signed and unsigned, 32 and 64
 * bits, every sign combination, values near the ends of the range, and
 * the shapes that must NOT be paired: the divisor or the dividend changed
 * between the two, the remainder before the quotient, a different width
 * or signedness. Each result is checked against one computed through
 * volatiles, which nothing combines. */
// expect-exit: 42
typedef long long ll;
typedef unsigned long long ull;

static volatile int vi; static volatile unsigned vu;
static volatile ll vl; static volatile ull vul;

__attribute__((noinline)) void s32(int a, int b, int *q, int *r) { *q = a / b; *r = a % b; }
__attribute__((noinline)) void u32(unsigned a, unsigned b, unsigned *q, unsigned *r) { *q = a / b; *r = a % b; }
__attribute__((noinline)) void s64(ll a, ll b, ll *q, ll *r) { *q = a / b; *r = a % b; }
__attribute__((noinline)) void u64(ull a, ull b, ull *q, ull *r) { *q = a / b; *r = a % b; }
__attribute__((noinline)) int bchanged(int a, int b) { int q = a / b; b += 3; int r = a % b; return q * 1000 + r; }
__attribute__((noinline)) int achanged(int a, int b) { int q = a / b; a -= 7; int r = a % b; return q * 1000 + r; }
__attribute__((noinline)) int modfirst(int a, int b) { int r = a % b; int q = a / b; return q * 1000 + r; }
__attribute__((noinline)) ll mixed(int a, int b) { int q = a / b; unsigned r = (unsigned)a % (unsigned)b; ll q2 = (ll)a / b; return (ll)q * 7 + (ll)r * 3 + q2; }
__attribute__((noinline)) unsigned digits(unsigned v, unsigned base)
{ unsigned s = 0; while (v) { unsigned d = v % base; v = v / base; s = s * 31 + d; } return s; }

static int ref_s32q(int a, int b) { vi = a; int x = vi; vi = b; return x / vi; }
static int ref_s32r(int a, int b) { vi = a; int x = vi; vi = b; return x % vi; }

int main(void)
{
    int bad = 0;
    static const int sv[] = { 0, 1, -1, 7, -7, 100, -100, 2147483647, -2147483647 - 1, 12345, -98765 };
    static const int dv[] = { 1, -1, 2, -2, 3, -3, 7, -7, 1000, -1000, 2147483647 };
    for (int i = 0; i < 11; i++)
        for (int j = 0; j < 11; j++) {
            int a = sv[i], b = dv[j], q, r;
            if (a == -2147483647 - 1 && b == -1) continue;   /* undefined */
            s32(a, b, &q, &r);
            if (q != ref_s32q(a, b) || r != ref_s32r(a, b)) bad |= 1;
            unsigned uq, ur; vu = (unsigned)b;
            u32((unsigned)a, (unsigned)b, &uq, &ur);
            if (uq != (unsigned)a / vu || ur != (unsigned)a % vu) bad |= 2;
            ll lq, lr; vl = b;
            s64((ll)a * 1000003, b, &lq, &lr);
            if (lq != ((ll)a * 1000003) / vl || lr != ((ll)a * 1000003) % vl) bad |= 4;
            ull ulq, ulr; vul = (ull)(ll)b;
            u64((ull)(ll)a * 77, (ull)(ll)b, &ulq, &ulr);
            if (ulq != ((ull)(ll)a * 77) / vul || ulr != ((ull)(ll)a * 77) % vul) bad |= 8;
        }
    if (bchanged(100, 7) != 14 * 1000 + 0) bad |= 16;       /* 100 % 10 */
    if (achanged(100, 7) != 14 * 1000 + 2) bad |= 32;       /* 93 % 7 */
    if (modfirst(-100, 7) != -14 * 1000 + -2) bad |= 64;
    vi = -9; vu = 4;
    if (mixed(vi, (int)vu) != (ll)(-2) * 7 + ((unsigned)-9 % 4u) * 3 + (-2)) bad |= 128;
    unsigned want = 0, v = 4000000000u;
    while (v) { vu = v; unsigned d = vu % 10u; v = vu / 10u; want = want * 31 + d; }
    if (digits(4000000000u, 10u) != want) bad |= 256;
    return bad ? 100 + (bad & 0x7f) + (bad >> 7) : 42;
}
