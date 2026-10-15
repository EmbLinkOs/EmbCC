/* GNU's integer complex types, `_Complex int` and the rest, and the
 * integer imaginary constant `2i`. EmbCC refused both ("invalid _Complex
 * type"); they are now pairs of the integer, with * and / in integer
 * arithmetic, as GCC computes them. */
// expect-exit: 42

typedef _Complex int ci;

static int bad;

static void eq(ci z, int re, int im, int bit)
{
    if (__real__ z != re || __imag__ z != im)
        bad |= bit;
}

__attribute__((noinline)) static ci mk(int a, int b)
{
    ci z;
    __real__ z = a;
    __imag__ z = b;
    return z;
}

_Complex int g = 3 + 4i;               /* folded, stored as two ints */
_Complex unsigned short gs = 5i;

int main(void)
{
    ci a = mk(3, 4), b = mk(1, -2);
    eq(a + b, 4, 2, 1);
    eq(a - b, 2, 6, 1);
    eq(a * b, 11, -2, 2);              /* (3+4i)(1-2i) */
    eq(a / b, -1, 2, 2);               /* (3+4i)/(1-2i), truncated */
    eq(a / 2, 1, 2, 2);
    eq(-a, -3, -4, 4);
    eq(~a, 3, -4, 4);                  /* the conjugate */
    eq(5 + a * 3, 14, 12, 4);
    ci c = a;
    c += b;
    c *= b;
    c++;
    eq(c, 9, -6, 8);
    if (!(a == mk(3, 4)) || !(a != b) || !(_Bool)a || (_Bool)mk(0, 0))
        bad |= 16;
    eq(2 + 3i, 2, 3, 32);
    eq(a * 1i, -4, 3, 32);
    eq(g, 3, 4, 64);
    if (__real__ gs != 0 || __imag__ gs != 5)
        bad |= 64;
    _Complex short s = a;              /* converted part by part */
    _Complex long long ll = mk(1, 2);
    ll = ll * mk(1000, 1000) * 1000;
    if (__real__ s != 3 || __imag__ s != 4 ||
        __real__ ll != -1000000 || __imag__ ll != 3000000)
        bad |= 128;
    _Complex double d = a;
    if (__real__ d != 3.0 || __imag__ d != 4.0)
        bad |= 128;
    return bad ? bad : 42;
}
