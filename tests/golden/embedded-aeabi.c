/* What clang and GCC call the ARM run-time ABI's helpers for, compiled by
 * clang and linked against EmbCC's lib/rt (thumb-aeabi.sh): 64-bit
 * division and remainder, block copy, move, fill and clear, soft-float
 * arithmetic, comparisons and conversions, 64-bit multiply and shifts.
 * The host runs the same source for the reference. */
extern void writec(int c);
extern void puts_(const char *s);
extern void putn(long v);

#define NI __attribute__((noinline))
typedef unsigned long long u64;
typedef long long s64;

static void hx(u64 v)
{
    for (int i = 60; i >= 0; i -= 4)
        writec("0123456789abcdef"[(int)((v >> i) & 15)]);
    writec(' ');
}

NI static u64 udiv(u64 a, u64 b) { return a / b; }
NI static u64 umod(u64 a, u64 b) { return a % b; }
NI static s64 sdiv(s64 a, s64 b) { return a / b; }
NI static s64 smod(s64 a, s64 b) { return a % b; }
NI static unsigned udiv32(unsigned a, unsigned b) { return a / b; }
NI static int sdiv32(int a, int b) { return a / b; }
NI static int smod32(int a, int b) { return a % b; }
NI static u64 mul64(u64 a, u64 b) { return a * b; }
NI static u64 shl(u64 a, int n) { return a << n; }
NI static u64 shr(u64 a, int n) { return a >> n; }
NI static s64 sar(s64 a, int n) { return a >> n; }

struct blk { int w[13]; char c[3]; };
NI static void copy(struct blk *d, const struct blk *s) { *d = *s; }
NI static void clear(struct blk *d) { *d = (struct blk){ 0 }; }
NI static void move(char *b, int n) { __builtin_memmove(b + 3, b, (unsigned)n); }
NI static void set(char *b, int c, int n) { __builtin_memset(b, c, (unsigned)n); }
NI static void cpy(char *d, const char *s, int n) { __builtin_memcpy(d, s, (unsigned)n); }
static volatile int five = 5, eleven = 11;   /* lengths clang cannot see */

NI static double dop(double a, double b, int k)
{
    switch (k) {
    case 0: return a + b; case 1: return a - b; case 2: return a * b;
    default: return a / b;
    }
}
NI static float fop(float a, float b, int k)
{
    switch (k) {
    case 0: return a + b; case 1: return a - b; case 2: return a * b;
    default: return a / b;
    }
}
NI static int dcmp(double a, double b)
{
    return (a == b) | (a < b) << 1 | (a <= b) << 2 | (a > b) << 3 |
           (a >= b) << 4 | (a != a || b != b) << 5;
}
NI static int fcmp(float a, float b)
{
    return (a == b) | (a < b) << 1 | (a <= b) << 2 | (a > b) << 3 |
           (a >= b) << 4 | (a != a || b != b) << 5;
}

static volatile double zero;

int main(void)
{
    static const u64 n[] = { 0, 1, 7, 1000000007ULL, 0x123456789abcdef0ULL,
                             0xffffffffffffffffULL, 0x8000000000000000ULL };
    static const u64 d[] = { 1, 3, 10, 0xffffffffULL, 0x100000001ULL,
                             0x7fffffffffffffffULL };
    for (unsigned i = 0; i < sizeof n / sizeof n[0]; i++) {
        for (unsigned j = 0; j < sizeof d / sizeof d[0]; j++) {
            hx(udiv(n[i], d[j])); hx(umod(n[i], d[j]));
            /* never 0 nor -1: no division by zero, no INT64_MIN / -1 */
            hx((u64)sdiv((s64)n[i], (s64)d[j] - 5));
            hx((u64)smod((s64)n[i], (s64)d[j] - 5));
        }
        hx(mul64(n[i], 0x9e3779b97f4a7c15ULL));
        hx(shl(n[i], (int)i * 9)); hx(shr(n[i], (int)i * 9 + 1));
        hx((u64)sar((s64)n[i], (int)i * 9 + 2));
        writec('\n');
    }
    putn((long)udiv32(4000000000u, 7u)); putn(sdiv32(-1000, 7));
    putn(smod32(-1000, 7)); writec('\n');

    struct blk a, b;
    for (int i = 0; i < 13; i++) a.w[i] = i * 1000003;
    a.c[0] = 'x'; a.c[1] = 'y'; a.c[2] = 'z';
    copy(&b, &a);
    putn(b.w[12]); writec(b.c[2]);
    clear(&b);
    putn(b.w[5] + b.c[1]);
    char buf[40];
    for (int i = 0; i < 40; i++) buf[i] = (char)('a' + i % 26);
    move(buf, 30); set(buf + 33, '#', five); cpy(buf + 1, buf + 20, eleven);
    for (int i = 0; i < 40; i++) writec(buf[i]);
    writec('\n');

    static const double dv[] = { 0.0, -0.0, 1.5, -2.25, 1e300, 3e-310, 7.0 };
    static const float fv[] = { 0.0f, -1.5f, 3.25f, 1e30f, 1e-40f, 7.0f };
    double nan = zero / zero;
    for (unsigned i = 0; i < sizeof dv / sizeof dv[0]; i++) {
        for (int k = 0; k < 4; k++) {
            double r = dop(dv[i], dv[(i + 2) % 7], k);
            union { double d; u64 u; } x = { r };
            hx(x.u);
        }
        putn(dcmp(dv[i], dv[(i + 3) % 7])); putn(dcmp(dv[i], nan));
        if (dv[i] > -1e9 && dv[i] < 1e9) {     /* in range: defined */
            putn((long)(int)dv[i]); putn((long)(long long)(dv[i] * 1e3));
        }
        writec('\n');
    }
    for (unsigned i = 0; i < sizeof fv / sizeof fv[0]; i++) {
        for (int k = 0; k < 4; k++) {
            float r = fop(fv[i], fv[(i + 1) % 6], k);
            union { float f; unsigned u; } x = { r };
            hx(x.u);
        }
        putn(fcmp(fv[i], fv[(i + 2) % 6]));
        if (fv[i] >= 0 && fv[i] < 4e9f)
            putn((long)(unsigned)fv[i]);
        union { double d; u64 u; } y = { (double)fv[i] };
        hx(y.u);
        writec('\n');
    }
    static const long long iv[] = { 0, -1, 123456789, -9007199254740993LL };
    for (unsigned i = 0; i < 4; i++) {
        union { double d; u64 u; } x = { (double)iv[i] };
        union { float f; unsigned u; } y = { (float)iv[i] };
        union { double d; u64 u; } z = { (double)(unsigned long long)iv[i] };
        union { float f; unsigned u; } w = { (float)(int)(iv[i] & 0x7fffffff) };
        hx(x.u); hx(y.u); hx(z.u); hx(w.u);
        writec('\n');
    }
    puts_("==END==\n");
    return 0;
}
