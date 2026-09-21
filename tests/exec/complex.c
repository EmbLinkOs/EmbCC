/* C99 complex arithmetic — float, double and long double _Complex — on both
 * targets: construction from imaginary constants, + - * / between complex
 * and real operands (Annex G: a real operand combines part by part), * and /
 * of two complex values through libgcc as gcc does (so infinities come out
 * right), ==/!=, negation, GNU ~ (conjugate) and __real__/__imag__ as
 * lvalues, compound assignment and ++, every conversion (real <-> complex,
 * complex -> integer and _Bool, between precisions), conditions, calls and
 * returns (the struct ABI both targets use for complex), arrays, structs,
 * static initializers, and a libm call.
 */
// expect-exit: 42
double cabs(double _Complex z);

struct S { int tag; double _Complex z; };

static double _Complex conjd(double _Complex z) { return ~z; }
static float _Complex scalef(float _Complex z, float k) { return z * k; }
static long double _Complex mulld(long double _Complex a, long double _Complex b)
{
    return a * b;
}
static double _Complex sum3(double _Complex a, double _Complex b,
                            double _Complex c)
{
    return a + b + c;
}

static double _Complex table[] = { 1.0 + 2.0i, -3.5i, 4, 2.0 * (1.5 - 0.5i) };
static const long double _Complex lt = 0.1L + 0.2Li;

int main(void)
{
    if (sizeof(float _Complex) != 8 || sizeof(double _Complex) != 16 ||
        sizeof(long double _Complex) != 32)
        return 1;
    if (_Alignof(double _Complex) != 8 || _Alignof(long double _Complex) != 16)
        return 2;

    double _Complex a = 1.0 + 2.0i, b = 3.0 + 4.0i;
    double _Complex c = a * b;                         /* (1+2i)(3+4i) = -5+10i */
    if (__real__ c != -5 || __imag__ c != 10) return 3;
    double _Complex q = c / b;                         /* back to 1+2i */
    if (__real__ q != 1 || __imag__ q != 2) return 4;
    if (a + b != 4.0 + 6.0i || b - a != 2.0 + 2.0i) return 5;
    if (a == b || !(a != b) || !(a == 1.0 + 2.0i)) return 6;

    /* a real operand combines part by part */
    double _Complex r = a * 2.0 + 0.5;                 /* 2.5 + 4i */
    if (__real__ r != 2.5 || __imag__ r != 4) return 7;
    r = 10.0 - a;                                      /* 9 - 2i */
    if (__real__ r != 9 || __imag__ r != -2) return 8;
    r = a / 2;                                         /* 0.5 + 1i */
    if (__real__ r != 0.5 || __imag__ r != 1) return 9;
    r = 5.0 / (1.0 + 2.0i);                            /* 1 - 2i */
    if (__real__ r != 1 || __imag__ r != -2) return 10;

    /* unary, conjugate, parts as lvalues */
    double _Complex n = -a;
    if (__real__ n != -1 || __imag__ n != -2) return 11;
    if (conjd(a) != 1.0 - 2.0i) return 12;
    __real__ n = 7;
    __imag__ n += 1;
    if (n != 7.0 - 1.0i) return 13;

    /* compound assignment and ++ */
    double _Complex z = 1.0i;
    z += 2;
    z *= 1.0i;                                         /* (2+i)i = -1 + 2i */
    z -= 1.0i;
    z /= 2;                                            /* -0.5 + 0.5i */
    if (z != -0.5 + 0.5i) return 14;
    z++;
    ++z;
    if (z != 1.5 + 0.5i) return 15;
    double _Complex arr[3] = { 1, 2, 3 };
    double _Complex *p = arr;
    *p++ += 1.0i;
    p[1] *= 2;
    if (arr[0] != 1.0 + 1.0i || arr[2] != 6) return 16;

    /* conversions */
    int k = (int)(3.75 + 9.0i);                        /* the real part */
    if (k != 3) return 17;
    _Bool bz = 0.0 + 0.0i, bi = 0.0 + 1.0i;
    if (bz || !bi) return 18;
    if (1.0i) {} else return 19;                       /* condition */
    volatile double _Complex zero = 0;
    if (zero) return 20;
    while (!zero) break;
    float _Complex f = 1.5f + 2.5fi;
    double _Complex d = f;                             /* exact widening */
    if (d != 1.5 + 2.5i) return 21;
    f = (float _Complex)(1.0 / 3.0 + 1.0i);
    if (__real__ f != (float)(1.0 / 3.0)) return 22;
    double re = b;                                     /* drops the imaginary */
    if (re != 3) return 23;
    if (scalef(f, 2.0f) != 2 * f) return 24;

    /* long double _Complex (x87 st0/st1 returns on x86-64) */
    long double _Complex la = 1.0L + 2.0Li, lb = 3.0L - 1.0Li;
    long double _Complex lc = mulld(la, lb);           /* 5 + 5i */
    if (__real__ lc != 5 || __imag__ lc != 5) return 25;
    if (lc / 5.0L != 1.0L + 1.0Li) return 26;        /* exact: by a real */
    long double _Complex err = lc / lb - la;           /* libgcc's division */
    if (__real__ err * __real__ err + __imag__ err * __imag__ err > 1e-30L)
        return 34;
    if (__real__ lt != 0.1L || __imag__ lt != 0.2L) return 27;

    /* Annex G: an infinite operand gives an infinite result, not NaN */
    volatile double zr = 0.0;
    double inf = 1.0 / zr;
    double _Complex big = inf + 0.0i;
    double _Complex prod = big * (1.0 + 1.0i);
    if (__real__ prod != inf || __imag__ prod != inf) return 28;

    /* calls, structs, statics, libm */
    if (sum3(a, b, 1.0i) != 4.0 + 7.0i) return 29;
    struct S s = { 1, 2.0 - 3.0i };
    struct S t = s;
    if (t.z != 2.0 - 3.0i || t.tag != 1) return 30;
    if (table[0] != a || table[1] != -3.5i || table[2] != 4 ||
        table[3] != 3.0 - 1.0i)
        return 31;
    if (cabs(3.0 + 4.0i) != 5.0) return 32;
    double _Complex tern = k > 2 ? a : 7;               /* ?: converts the arms */
    if (tern != a) return 33;
    return 42;
}
