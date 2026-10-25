/* `return f_of(g(x))`, f_of a union pun inlined to nothing: the call's
 * u32 is returned as the function's float. A sibling call there must
 * not be made -- the callee leaves its value in an integer register
 * where this function's caller reads a float one -- and at x86-64 -Os
 * one was: lib/rt/half.c's __truncdfhf2 returned garbage for every
 * double. Also the other way round, and a width that differs. */
// expect-exit: 42

typedef unsigned int u32;
typedef unsigned long long u64;

static float f_of(u32 u) { union { float f; u32 u; } x; x.u = u; return x.f; }
static u32 u_of(float f) { union { float f; u32 u; } x; x.f = f; return x.u; }
static double d_of(u64 u) { union { double d; u64 u; } x; x.u = u; return x.d; }

__attribute__((noinline)) static u32 g(u64 v) { return (u32)(v >> 7) ^ 0x3f800001u; }
__attribute__((noinline)) static float h(u32 v) { return (float)v * 0.5f; }
__attribute__((noinline)) static u64 k(u32 v) { return (u64)v << 33 | 5; }

__attribute__((noinline)) float as_float(u64 v) { return f_of(g(v)); }
__attribute__((noinline)) u32 as_bits(u32 v) { return u_of(h(v)); }
__attribute__((noinline)) double as_double(u32 v) { return d_of(k(v)); }

int main(void)
{
    volatile u64 seed = 0x0123456789abcdefull;
    volatile u32 small = 1234;
    int bad = 0;
    bad |= (u_of(as_float(seed)) != g(seed)) << 0;
    bad |= (as_bits(small) != u_of((float)small * 0.5f)) << 1;
    if (sizeof(double) == 8) {           /* (AVR's and RX's are four bytes) */
        double d = as_double(small);
        union { double d; u64 u; } x;
        x.d = d;
        bad |= (x.u != k(small)) << 2;
    }
    return bad ? bad : 42;
}
