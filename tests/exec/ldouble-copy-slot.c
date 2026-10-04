/* A long double parameter copied once and not read again, while two
 * other long double locals are assigned after the copy: the copy has to
 * keep the parameter's value.
 *
 * x86-64 gives a 16-byte copy of a value that never changes no slot of
 * its own -- it reads the source's -- and gives locals whose lives do not
 * overlap one slot between them. Together, the parameter's slot went to
 * `a` and then `t` while the copy still lived there, so roundl(-2.5L)
 * returned +3: the final `x < 0` read `a`. -O2 had it; lib/libc, built at
 * -O1, did not show it until -O1 began removing the second load of x. */
// expect-exit: 42
__attribute__((noinline)) static long double trunc_ld(long double x)
{
    return (long double)(long long)x;
}

__attribute__((noinline)) static long double round_ld(long double x)
{
    long double a = x < 0 ? -x : x;
    long double t = trunc_ld(a);
    if (a - t >= 0.5L)
        t += 1.0L;
    return x < 0 ? -t : t;
}

int main(void)
{
    if (round_ld(2.5L) != 3.0L)
        return 1;
    if (round_ld(-2.5L) != -3.0L)
        return 2;
    if (round_ld(-7.25L) != -7.0L)
        return 3;
    return 42;
}
