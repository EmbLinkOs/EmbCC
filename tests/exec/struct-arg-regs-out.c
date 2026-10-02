/* A struct the SysV ABI would pass in registers travels whole on the
 * stack when not enough are left for all its eightbytes -- and takes none
 * of them. EmbCC's x86-64 callee read it from the next "register"
 * regardless: the seventh-place struct{short} below came out of r9, which
 * held the sixth argument. Its caller did the right thing, so a call from
 * gcc's code failed the same way as a call from EmbCC's. Integer-class,
 * SSE-class, and mixed ones where only one class has run out. */
// expect-exit: 42
struct s2 { short m; };
struct s16 { long a, b; };
struct d1 { double d; };
struct mix { long l; double d; };
__attribute__((noinline)) static long ints(long a, long b, long c, long d, long e, long f,
                                           struct s2 s, struct s16 t)
{ return a + b + c + d + e + f + s.m * 100 + t.a * 10000 + t.b; }
__attribute__((noinline)) static double dbls(double a, double b, double c, double d,
                                             double e, double f, double g, double h,
                                             struct d1 x, double y)
{ return a + b + c + d + e + f + g + h + x.d * 100 + y * 1000; }
__attribute__((noinline)) static double mixed(long a, long b, long c, long d, long e,
                                              struct mix m, long z)
{ return (double)(a + b + c + d + e) + m.l * 10 + m.d * 1000 + (double)z * 100000; }
int main(void)
{
    struct s2 s = { 7 };
    struct s16 t = { 3, 4 };
    struct d1 x = { 5 };
    struct mix m = { 2, 9 };
    int bad = 0;
    if (ints(1, 2, 3, 4, 5, 6, s, t) != 21 + 700 + 30004) bad |= 1;
    if (dbls(1, 2, 3, 4, 5, 6, 7, 8, x, 6) != 36 + 500 + 6000) bad |= 2;
    /* five integer registers used: m needs one more integer and one SSE,
     * which fit -- then z takes the sixth integer register */
    if (mixed(1, 2, 3, 4, 5, m, 8) != 15 + 20 + 9000 + 800000) bad |= 4;
    return bad ? bad : 42;
}
