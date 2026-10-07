/* Dynamic stack allocation on AVR: variable-length arrays, alloca, and
 * locals aligned beyond what the stack promises (which is nothing here).
 * Each moves sp below the frame, which stays at Y; a call's stack
 * arguments must still land just above the return address, so they are
 * copied down to the new sp at every call (src/arch/avr/codegen.c,
 * IR_CALL). The cases below are those paths: stack arguments after a VLA,
 * direct, indirect and variadic; a VLA scope in a loop, which overflows
 * the ATmega328P's 2 KiB if its sp is not restored each trip; recursion;
 * values that must survive the sp moves around a call. The host runs the
 * same program, and tests/golden/avr-alloca.sh compares the two. */
#include <stdarg.h>
void puts_(const char *s);
void putn(long v);

/* twelve ints: r25 down to r8 holds nine, the last three are on the stack */
__attribute__((noinline)) long twelve(int a, int b, int c, int d, int e,
                                      int f, int g, int h, int i, int j,
                                      int k, int l)
{
    return a + 2L * b + 3L * c + 4L * d + 5L * e + 6L * f + 7L * g +
           8L * h + 9L * i + 10L * j + 11L * k + 12L * l;
}

/* every argument of a variadic callee is on the stack */
__attribute__((noinline)) long vsum(int n, ...)
{
    va_list ap;
    long s = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        s = s * 3 + va_arg(ap, int);
    va_end(ap);
    return s;
}

static long (*volatile fp)(int, int, int, int, int, int, int, int, int,
                           int, int, int) = twelve;

/* a VLA, then calls passing arguments on the stack */
__attribute__((noinline)) long vla_calls(int n)
{
    char a[n];
    for (int i = 0; i < n; i++)
        a[i] = (char)(i * 7 + 1);
    long r = twelve(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8],
                    a[9], a[10], a[n - 1]);
    long q = fp(1, 2, 3, 4, 5, 6, 7, 8, 9, a[3], a[4], a[5]);
    long v = vsum(6, a[1], a[2], a[3], 40, 50, a[n - 2]);
    long s = 0;
    for (int i = 0; i < n; i++)          /* the array survived the calls */
        s += a[i];
    return r * 7 + q * 5 + v * 3 + s;
}

/* a VLA scope per trip: 80 trips of 32 to 40 bytes is about 2.9 KB,
 * more than the part's 2 KiB, so each trip must give its block back */
__attribute__((noinline)) long vla_loop(int trips)
{
    long t = 0;
    for (int k = 0; k < trips; k++) {
        int v[20];
        int w[(k % 5) + 16];             /* this one is variable */
        for (int i = 0; i < (k % 5) + 16; i++)
            w[i] = k + i;
        v[0] = w[(k % 5) + 15];
        t += v[0] + twelve(w[0], w[1], 0, 0, 0, 0, 0, 0, 0, w[2], w[3], k);
    }
    return t;
}

/* each frame its own block */
__attribute__((noinline)) int vla_rec(int n)
{
    int b[n + 1];
    for (int i = 0; i <= n; i++)
        b[i] = n * 10 + i;
    int below = n ? vla_rec(n - 1) : 0;
    int s = 0;
    for (int i = 0; i <= n; i++)
        s += b[i];
    return s + below;
}

/* alloca: freed at the return, so a loop of them adds up */
__attribute__((noinline)) long alloca_sum(int n)
{
    long s = 0;
    for (int k = 1; k <= n; k++) {
        unsigned char *p = __builtin_alloca(k);
        for (int i = 0; i < k; i++)
            p[i] = (unsigned char)(k + i);
        s += p[k - 1] + twelve(p[0], 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, p[k - 1]);
    }
    return s;
}

/* aligned locals: addresses at their alignment, values through calls */
__attribute__((noinline)) long aligned_locals(int x)
{
    _Alignas(8) long l = x * 3L;
    __attribute__((aligned(16))) char buf[5] = { 1, 2, 3, 4, 5 };
    _Alignas(4) int k = x + 1;
    struct { char c; int i; } __attribute__((aligned(8))) st = { 9, 300 };
    int ok = ((unsigned)&l % 8 == 0) + ((unsigned)buf % 16 == 0) +
             ((unsigned)&k % 4 == 0) + ((unsigned)&st % 8 == 0);
    l += twelve(buf[0], buf[1], buf[2], buf[3], buf[4], k, st.c, 0, 0, 0,
                st.i, (int)l);
    k *= 2;
    return l + k + ok * 1000L;
}

/* twelve's shape without its multiplies, so 400 calls stay quick at -O0 */
__attribute__((noinline)) int twelve_sum(int a, int b, int c, int d, int e,
                                         int f, int g, int h, int i, int j,
                                         int k, int l)
{
    return a + b + c + d + e + f + g + h + i + j + k - l;
}

/* storage carved once at the entry, then many calls with stack
 * arguments: 400 of them would leak 2400 bytes, more than the part has,
 * if sp were not given back after each */
__attribute__((noinline)) long many_calls(int n)
{
    _Alignas(4) int base = 3;
    long t = 0;
    for (int i = 0; i < n; i++)
        t += twelve_sum(base, i, 0, 0, 0, 0, 0, 0, 0, 1, 2, i & 7);
    return t + base;
}

__attribute__((noinline)) int fill_sum(volatile char *p, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        p[i] = (char)(i + 1);
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

/* alloca and nothing else: from -O1 there is no frame, and the epilogue
 * must still put sp back where the return address is */
__attribute__((noinline)) int tiny(int n)
{
    return fill_sum(__builtin_alloca(n), n);
}

int main(void)
{
    putn(vla_calls(12)); putn(vla_calls(20)); puts_("\n");
    putn(vla_loop(80)); puts_("\n");
    putn(vla_rec(6)); puts_("\n");
    putn(alloca_sum(10)); puts_("\n");
    putn(aligned_locals(7)); putn(aligned_locals(-20)); puts_("\n");
    putn(many_calls(400)); puts_("\n");
    putn(tiny(9)); putn(tiny(30)); puts_("\n");
    puts_("==END==\n");
    return 0;
}
