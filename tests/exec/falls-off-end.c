/* A function that returns a value but can reach its closing brace: valid
 * C while no caller uses the value (C17 6.9.1p12), so a warning
 * (--warn=return-type, E0008) and no longer an error; the result there is
 * 0 for an integer or a pointer. */
// expect-exit: 42
int printf(const char *, ...);
static int calls;
__attribute__((noinline)) static int sign(int n)
{
    if (n > 0) return 1;
    if (n < 0) return -1;
}
__attribute__((noinline)) static char *pick(int n)
{
    static char s[] = "x";
    calls++;
    if (n) return s;
}
__attribute__((noinline)) static double half(int n)
{
    if (n) return n / 2.0;
}
int main(void)
{
    int ok = 0;
    ok += sign(5) == 1 && sign(-3) == -1;
    sign(0);                    /* value unused: fine in C */
    ok += pick(1) != 0;
    pick(0);
    ok += half(4) == 2.0;
    half(0);
    printf("%d %d\n", ok, calls);
    return ok == 3 && calls == 2 ? 42 : ok;
}
