/* The value of x++ and x-- is x's old value at x's own width: a 64-bit
 * integer, a double, a float, a pointer, a narrow integer -- through a
 * local, a volatile local, a pointer, a member and an array element. */
// expect-exit: 42
struct s { long long q; double d; float f; unsigned short h; };

int main(void)
{
    volatile long long vq = 0x123456789LL;
    long long q = vq--;
    if (q != 0x123456789LL || vq != 0x123456788LL) return 1;
    long long q2 = 0x1ffffffffLL, *pq = &q2;
    long long r = (*pq)++;
    if (r != 0x1ffffffffLL || q2 != 0x200000000LL) return 2;
    unsigned long long u = 0xffffffffULL;
    unsigned long long w = u++;
    if (w != 0xffffffffULL || u != 0x100000000ULL) return 3;

    volatile double vd = 3.5;
    double e = vd--;
    if (e != 3.5 || vd != 2.5) return 4;
    double arr[2] = { 0.25, 1e10 };
    double a1 = arr[1]++;
    if (a1 != 1e10 || arr[1] != 1e10 + 1) return 5;

    struct s st = { -1, -0.5, 7.25f, 65535 };
    struct s *sp = &st;
    long long m = sp->q++;
    double md = sp->d++;
    float mf = st.f--;
    unsigned short mh = st.h++;
    if (m != -1 || st.q != 0) return 6;
    if (md != -0.5 || st.d != 0.5) return 7;
    if (mf != 7.25f || st.f != 6.25f) return 8;
    if (mh != 65535 || st.h != 0) return 9;

    char buf[4] = "abc";
    char *p = buf;
    char *p0 = p++;
    if (p0 != buf || *p != 'b') return 10;
    return 42;
}
