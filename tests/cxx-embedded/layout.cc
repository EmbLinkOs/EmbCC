// The data model, from both sides: what the C++ front end computes at
// compile time (sizeof, offsetof, alignof, and constant expressions read
// out of objects and string literals) against what the code it emits does
// at run time (array strides, member addresses, bit-fields stored and
// loaded). The two are computed apart -- the C++ front end lays a class
// out itself, and the C it lowers to is laid out again -- so a target
// whose alignment is capped (ColdFire 2, RX and TriCore 4, AVR 1), whose
// bit-fields straddle units or follow Microsoft's rules (RX), or whose
// bytes are big-endian (MIPS, SPARC, PowerPC, ColdFire), shows any
// difference here as a line the host does not print. Each line printed is
// one property; the host's clang++ build prints the same.
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

// ---- layout: compile time against run time ----------------------------
struct Bits {                 // straddles a capped unit; a :0
    char a;
    int b : 20;
    short c : 12;
    long long d : 40;
    char e;
    unsigned f : 3, : 0, g : 5;
};
struct Ms {                   // a run of one size, broken by another
    signed char a : 4;
    int b : 8;
    signed char c : 4;
    short d;
};
struct Wide { char c; long long x; double d; short s; };
struct Base { short id; virtual int kind() const { return 1; } };
struct Derived : Base {       // laid out explicitly: a base, a vptr
    char tag;
    int n : 7;
    unsigned m : 9;
    long long big;
    int kind() const override { return 2; }
};
struct Empty {};
struct WithEmpty : Empty { char c; int i; };

template <typename T> static int stride_ok()
{
    static T arr[3];
    return sizeof(T) == (size_t)((char *)&arr[2] - (char *)&arr[1]) &&
           alignof(T) <= sizeof(T) && sizeof(T) % alignof(T) == 0;
}

#define OFF_OK(T, m) \
    (offsetof(T, m) == (size_t)((char *)&probe_##T.m - (char *)&probe_##T))

static Bits probe_Bits;
static Ms probe_Ms;
static Wide probe_Wide;
static WithEmpty probe_WithEmpty;

static void layout()
{
    printf("stride %d %d %d %d %d %d %d\n", stride_ok<Bits>(), stride_ok<Ms>(),
           stride_ok<Wide>(), stride_ok<Derived>(), stride_ok<WithEmpty>(),
           stride_ok<wchar_t>(), stride_ok<char16_t>());
    printf("offsets %d %d %d %d %d %d %d\n", OFF_OK(Bits, e), OFF_OK(Ms, d),
           OFF_OK(Wide, x), OFF_OK(Wide, d), OFF_OK(Wide, s),
           OFF_OK(WithEmpty, c), OFF_OK(WithEmpty, i));
    Derived dv;
    printf("derived %d %d\n",
           (int)((char *)&dv.big - (char *)&dv) % (int)alignof(long long) == 0,
           (char *)&dv.tag - (char *)&dv >= (long)sizeof(void *));
    // bit-fields, written and read back through the lowered code
    Bits b = {};
    b.a = 'x'; b.b = -300000; b.c = 2000; b.d = -123456789012LL; b.e = 'y';
    b.f = 5; b.g = 17;
    printf("bits %c %d %d %lld %c %u %u\n", b.a, b.b, b.c, (long long)b.d,
           b.e, b.f, b.g);
    Ms m = {};
    m.a = -3; m.b = 100; m.c = 7; m.d = -2;
    printf("ms %d %d %d %d\n", m.a, m.b, m.c, m.d);
    dv.tag = 't'; dv.n = -40; dv.m = 300; dv.big = 1LL << 40;
    Base *bp = &dv;
    printf("derived bits %c %d %u %lld kind %d\n", dv.tag, dv.n, dv.m,
           (long long)dv.big, bp->kind());
}

// ---- the constant evaluator: objects as the target's bytes ------------
template <long long V> struct K { static constexpr long long v = V; };

constexpr long long sum16(const char16_t *s)
{
    long long r = 0;
    for (; *s; s++)
        r = (r * 131 + *s) % 1000000007;
    return r;
}
constexpr long long sum32(const char32_t *s)
{
    long long r = 0;
    for (; *s; s++)
        r = (r * 131 + (long long)*s) % 1000000007;
    return r;
}
constexpr long long sumw(const wchar_t *s)
{
    long long r = 0;
    for (; *s; s++)
        r = (r * 131 + (long long)*s) % 1000000007;
    return r;
}

struct Rec {                  // every width, stored and read back
    uint8_t b;
    uint16_t h;
    uint32_t w;
    uint64_t d;
    int16_t sh;
    float f;
    double x;
    unsigned lo : 3;
    int mid : 11;
    unsigned hi : 13;
};

constexpr Rec make_rec(int k)
{
    Rec r{};
    r.b = (uint8_t)(0xa5 + k);
    r.h = (uint16_t)(0xbeef + k);
    r.w = 0xdeadbeefu + (uint32_t)k;
    r.d = 0x0123456789abcdefULL + (uint64_t)k;
    r.sh = (int16_t)(-1234 - k);
    r.f = 1.5f * (float)k;
    r.x = 2.25 * k;
    r.lo = (unsigned)(5 + k) & 7;
    r.mid = -700 + k;
    r.hi = 6000u + (unsigned)k;
    return r;
}

constexpr long long fold(int k)
{
    Rec a[3] = { make_rec(k), make_rec(k + 1), make_rec(k + 2) };
    Rec c = a[1];             // copied as bytes
    a[2].h += 1;
    a[2].mid = a[2].mid * 2;
    return (long long)c.b + c.h + c.w + (long long)(c.d % 100000) + c.sh +
           (long long)(c.f * 4) + (long long)(c.x * 8) + c.lo + c.mid +
           c.hi + a[2].h + a[2].mid + (long long)(a[0].d >> 40);
}

constexpr int literal_bytes()
{
    // the units of a wide literal, read one at a time
    constexpr char16_t s16[] = u"ሴÿ";
    constexpr char32_t s32[] = U"\U0001f600z";
    return (s16[0] == 0x1234) + 2 * (s16[1] == 0xff) +
           4 * (s32[0] == 0x1f600) + 8 * (s32[1] == U'z');
}

static_assert(literal_bytes() == 15, "a wide literal's units");
static_assert(make_rec(3).h == 0xbef2 && make_rec(3).mid == -697,
              "an object's members");

static void consteval_()
{
    printf("u16 %lld u32 %lld w %lld\n", K<sum16(u"embedded été")>::v,
           K<sum32(U"\U0001f680 rtos")>::v, K<sumw(L"wide")>::v);
    printf("units %lld\n", K<literal_bytes()>::v);
    printf("fold %lld %lld\n", K<fold(0)>::v, K<fold(7)>::v);
    // and the same at run time, from the objects the program holds
    volatile int k = 7;
    printf("fold at run time %lld\n", fold(k));
}

// ---- a float's bytes: the sign is in the most significant one, first in
// memory on a big-endian target; RX's double is a float ----------------
static void floats()
{
    volatile float nf = -2.5f, pf = 3.0f, tiny = 1e-40f;
    volatile double nd = -0.0, pd = 7.0, zd = 0.0;
    printf("signbit %d %d %d %d\n", __builtin_signbit(nf) != 0,
           __builtin_signbit(pf) != 0, __builtin_signbit(nd) != 0,
           __builtin_signbit(pd) != 0);
    printf("isnormal %d %d %d %d\n", __builtin_isnormal(pf),
           __builtin_isnormal(tiny), __builtin_isnormal(pd),
           __builtin_isnormal(zd));
}

int main()
{
    layout();
    floats();
    consteval_();
    printf("==END==\n");
    return 0;
}
