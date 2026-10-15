/* Type names of function types: `int (int)` in __builtin_types_compatible_p,
 * typeof and sizeof, a parameter declared `int f(int)`, and a statement
 * that begins with _Generic (all refused as syntax errors before). */
// expect-exit: 42
int printf(const char *, ...);
typedef int F(int);
static int twice(int x) { return 2 * x; }
static int apply(int (int), int);           /* a parameter of function type */
static int apply(int f(int), int v) { return f(v); }
int main(void)
{
    int ok = 0;
    ok += sizeof(int (*)(int)) == sizeof(void *);
    ok += __builtin_types_compatible_p(int (int), F);
    ok += __builtin_types_compatible_p(int *(char, ...), int *(char, ...));
    ok += !__builtin_types_compatible_p(int (int), int (long));
    ok += apply(twice, 21) == 42;
    _Generic(&twice, int (*)(int): (void)0);
    __typeof__(int (int)) *fp = twice;
    ok += fp(4) == 8;
    printf("%d %d\n", (int)sizeof(void (void)), ok);
    return ok == 6 ? 42 : ok;
}
