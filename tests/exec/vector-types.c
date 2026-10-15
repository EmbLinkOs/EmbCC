/* GNU C's vector types: `typedef int v4si __attribute__((vector_size(16)))`.
 * EmbCC refused the attribute; a vector is now a struct of its lanes and
 * every operator is applied lane by lane, so each target runs them with
 * its own scalar code. Every lane checked here is the answer GCC gives. */
// expect-exit: 42

/* by lanes, so each has as many on every target -- int is two bytes
 * on AVR */
typedef int V __attribute__((vector_size(4 * sizeof(int))));
typedef unsigned U __attribute__((vector_size(4 * sizeof(unsigned))));
typedef float F __attribute__((vector_size(4 * sizeof(float))));
typedef short S __attribute__((vector_size(4 * sizeof(short))));
typedef unsigned char C __attribute__((vector_size(8)));
typedef signed char SC __attribute__((vector_size(4)));
typedef long long L __attribute__((vector_size(16)));
typedef __INT32_TYPE__ V2 __attribute__((vector_size(8)));
typedef __INT32_TYPE__ I4 __attribute__((vector_size(16)));  /* F's < */
#define vector __attribute__((vector_size(4 * sizeof(int))))

struct holder { char c; V v; };

static int bad;

static void eq(V v, int a, int b, int c, int d, int bit)
{
    if (v[0] != a || v[1] != b || v[2] != c || v[3] != d)
        bad |= bit;
}

__attribute__((noinline)) static V add(V a, V b) { return a + b; }

/* a leading attribute on a parameter, as a macro spells it */
__attribute__((noinline)) static int lanes(vector int t)
{
    return t[0] + t[1] + t[2] + t[3];
}

int main(void)
{
    V a = {1, 2, 3, 4}, b = {10, 20, 30, 40};

    /* the operators, of two vectors and of a vector and a scalar */
    eq(a + b, 11, 22, 33, 44, 1);
    eq(b / a - 9, 1, 1, 1, 1, 1);
    eq(b % (V){3, 3, 3, 3}, 1, 2, 0, 1, 1);
    eq((a & 3) | 8, 9, 10, 11, 8, 2);
    eq(a ^ b, 11, 22, 29, 44, 2);
    eq(a << a, 2, 8, 24, 64, 2);
    eq(b >> 1, 5, 10, 15, 20, 2);
    eq(100 - a * 2, 98, 96, 94, 92, 2);
    eq(-a, -1, -2, -3, -4, 4);
    eq(~a, -2, -3, -4, -5, 4);

    /* a comparison: -1 or 0 in each lane, a vector that converts to any
     * of its size (GCC's opaque type) */
    eq(a < (V){2, 2, 4, 4}, -1, 0, -1, 0, 8);
    U u = a == (V){1, 0, 3, 0};
    if (u[0] != (unsigned)-1 || u[1] != 0 || u[2] != (unsigned)-1)
        bad |= 8;

    /* compound assignment, ++, and a lane as an lvalue */
    V c = a;
    c += b;
    c++;
    c *= 2;
    eq(c, 24, 46, 68, 90, 16);
    c[2] = 99;
    int i = 3;
    c[i] = -7;
    eq(c, 24, 46, 99, -7, 16);
    if ((a + b)[1] != 22)
        bad |= 16;

    /* passed, returned, a member, an array element */
    eq(add(a, b), 11, 22, 33, 44, 32);
    struct holder h = { 'x', {5, 6, 7, 8} };
    h.v[0] = 50;
    eq(h.v, 50, 6, 7, 8, 32);
    V arr[2] = { {1, 1, 1, 1}, {2, 2, 2, 2} };
    arr[1] += arr[0];
    eq(arr[1], 3, 3, 3, 3, 32);
    if (sizeof(V) != 4 * sizeof(int) ||
        __builtin_offsetof(struct holder, v) % sizeof(V) % 8 ||
        lanes(a) != 10)
        bad |= 32;

    /* other lanes: float, short, unsigned char (wrapping), long long */
    F f = {1.5f, 2.5f, -3.0f, 4.0f};
    F g = f * 2 + 1;
    if (g[0] != 4.0f || g[2] != -5.0f)
        bad |= 64;
    I4 fc = f > (F){2, 2, 2, 2};
    if (fc[0] != 0 || fc[1] != -1 || fc[3] != -1)
        bad |= 64;
    S s = {1, -2, 3, -4};
    s = s * s;
    if (s[1] != 4 || s[3] != 16)
        bad |= 64;
    C ch = {250, 251, 252, 253, 254, 255, 0, 1};
    ch += 10;
    if (ch[0] != 4 || ch[5] != 9 || ch[6] != 10)
        bad |= 64;
    /* a scalar shifted by a vector is a lane first: (signed char)0x1f0 */
    int n = 0x1f0;
    SC sc = {1, 2, 3, 4};
    SC r = n >> sc;
    if (r[0] != -8 || r[1] != -4 || r[3] != -1)
        bad |= 64;
    L l = {1LL << 40, -5};
    l = l * 3;
    if (l[0] != 3LL << 40 || l[1] != -15)
        bad |= 64;

    /* casts keep the bytes: vector to vector, vector to an integer */
    U uu = (U)(a - 2);
    if (uu[0] != (unsigned)-1 || uu[3] != 2)
        bad |= 128;
    V2 v2 = {1, 2};
    long long bits = (long long)v2;
    V2 back = (V2)bits;
    if (back[0] != 1 || back[1] != 2 || bits == 0)
        bad |= 128;
    vector float vf = (vector float)a;   /* a type name led by attributes */
    if (sizeof vf != sizeof a)
        bad |= 128;

    /* the shuffles and the conversion */
    eq(__builtin_shuffle(a, (V){3, 2, 1, 0}), 4, 3, 2, 1, 256);
    eq(__builtin_shuffle(a, b, (V){0, 4, 1, 7}), 1, 10, 2, 40, 256);
    eq(__builtin_shufflevector(a, b, 7, 6, 0, 1), 40, 30, 1, 2, 256);
    eq(__builtin_convertvector(f, V), 1, 2, -3, 4, 256);

    return bad ? bad : 42;
}
