/* What tests/golden/thumb-m7-dp.sh disassembles: one function per double
 * operation the Cortex-M7's FPU does, so each instruction it expects has
 * a place to appear, and every helper it forbids a place to be called
 * from. Not run -- dp.c is what runs. */
double add(double a, double b) { return a + b; }
double sub(double a, double b) { return a - b; }
double mul(double a, double b) { return a * b; }
double quo(double a, double b) { return a / b; }
double neg(double a) { return -a; }
double mag(double a) { return __builtin_fabs(a); }
double root(double a) { return __builtin_sqrt(a); }
int lt(double a, double b) { return a < b; }
int eq(double a, double b) { return a == b; }
int br(double a, double b) { if (a >= b) return 3; return 5; }
double from_i(int i) { return i; }
double from_u(unsigned u) { return u; }
int to_i(double d) { return (int)d; }
unsigned to_u(double d) { return (unsigned)d; }
double widen(float f) { return f; }
float narrow(double d) { return (float)d; }
double pick(int c, double a, double b) { return c ? a * 2.0 : b - 1.0; }
/* the 64-bit integer conversions, which VFP has no instruction for and
 * which therefore stay calls */
long long to_ll(double d) { return (long long)d; }
double from_ll(long long x) { return (double)x; }

/* Arguments: a double, a float and a double go in d0, s2 (back-filled
 * into d1's low half) and d2, and the result comes back in d0 -- with no
 * double crossing to the core registers on the way. */
double callee(double a, float b, double c);
double caller(double x, double y)
{
    return callee(x * y, 1.5f, x - y) * 2.0 + y;
}
