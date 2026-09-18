/* ++ and -- on a floating lvalue add or subtract 1.0 (C11 6.5.2.4, 6.5.3.1)
 * — as a float operation, not an integer 1 added to the bit pattern (which
 * nudges 1.5 to 1.5000000000000002). Pre and post forms, through a local, a
 * pointer, a struct member and an array element, across zero, and at a
 * magnitude where adding 1.0 rounds away entirely.
 */
// expect-exit: 42
struct s { double d; float f; };

int main(void)
{
    volatile double d = 1.5;
    d++;
    ++d;
    double e = d--;
    if (d != 2.5 || e != 3.5) return 1;
    volatile float f = -0.5f;
    f++;                               /* crosses zero: 0.5 */
    float g = ++f;                     /* 1.5 */
    if (f != 1.5f || g != 1.5f) return 2;
    --f; --f; --f;
    if (f != -1.5f) return 3;

    double arr[3] = { 0.25, 0.5, 0.75 };
    double *p = &arr[1];
    (*p)++;
    arr[2]--;
    if (arr[1] != 1.5 || arr[2] != -0.25) return 4;
    struct s st = { 10.0, 20.0f };
    struct s *sp = &st;
    sp->d++;
    st.f--;
    if (st.d != 11.0 || st.f != 19.0f) return 5;

    volatile double big = 1e16;        /* the spacing here is 2.0 */
    big++;
    if (big != 1e16) return 6;         /* 1e16 + 1 rounds back to 1e16 */
    volatile float fbig = 16777216.0f; /* 2^24 */
    fbig++;
    if (fbig != 16777216.0f) return 7;
    return 42;
}
