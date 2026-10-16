/* __builtin_isgreater and kin (each operand evaluated once), the
 * overflow_p tests, classify_type and the per-type isinff/isnanf/finite. */
// expect-exit: 42
int printf(const char *, ...);
static int calls;
static double nanv(void) { calls++; return __builtin_nan(""); }
enum col { RED, GREEN };
int main(void)
{
    double a = 1.5, b = 2.5;
    float fz = 0.0f;
    int ok = 0;
    ok += __builtin_isgreater(b, a) && !__builtin_isgreater(a, b);
    ok += __builtin_isless(a, b) && __builtin_islessequal(a, a) && __builtin_isgreaterequal(b, b);
    ok += __builtin_islessgreater(a, b) && !__builtin_islessgreater(a, a);
    ok += __builtin_isunordered(nanv(), a) && !__builtin_isunordered(a, b) && calls == 1;
    ok += !__builtin_islessgreater(nanv(), a) && calls == 2;
    ok += __builtin_isinff(1.0f / fz) && !__builtin_isnanf(1.0f) && __builtin_finite(a);
    ok += __builtin_mul_overflow_p(1 << 20, 1 << 12, (int)0) && !__builtin_add_overflow_p(1, 2, (char)0);
    ok += __builtin_sub_overflow_p(0, 1, (unsigned)0);
    char c = 0; enum col e = RED; struct { int x; } s; int arr[3]; float fl = 0;
    ok += __builtin_classify_type(c) == 1 && __builtin_classify_type(e) == 1 && __builtin_classify_type(s) == 12;
    ok += __builtin_classify_type(arr) == 5 && __builtin_classify_type(fl) == 8 && __builtin_classify_type(&c) == 5;
    printf("%d %d\n", ok, calls);
    return ok == 10 ? 42 : ok;
}
