typedef unsigned long u32;
extern void writec(int);
static u32 st = 362436069UL;
static u32 xs(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }
/* long division by shifts and subtractions: no / or % anywhere */
static unsigned rdiv(unsigned x, unsigned d, unsigned *r)
{
    unsigned q = 0, rem = 0;
    for (int b = 15; b >= 0; b--) {
        rem = (rem << 1) | ((x >> b) & 1);
        if (rem >= d) { rem -= d; q |= 1u << b; }
    }
    *r = rem;
    return q;
}
#define D(n) __attribute__((noinline)) static unsigned q##n(unsigned x) { return x / n; } \
             __attribute__((noinline)) static unsigned r##n(unsigned x) { return x % n; } \
             __attribute__((noinline)) static unsigned char c##n(unsigned char x) { return x / n; }
D(3) D(5) D(7) D(10) D(11) D(13) D(60) D(100) D(127) D(255) D(1000) D(9999) D(32767) D(65535)
typedef unsigned (*fq)(unsigned);
typedef unsigned char (*fc)(unsigned char);
static const struct { unsigned d; fq q, r; fc c; } t[] = {
#define T(n) { n, q##n, r##n, c##n }
    T(3), T(5), T(7), T(10), T(11), T(13), T(60), T(100), T(127), T(255), T(1000), T(9999), T(32767), T(65535) };
static const unsigned edge[8] = { 0, 1, 9, 10, 0x7fff, 0x8000, 0xfffe, 0xffff };
static void say(const char *m) { for (; *m; m++) writec(*m); }
int main(void)
{
    int bad = 0;
    for (int i = 0; i < 300; i++) {
        unsigned x = i < 8 ? edge[i] : (unsigned)xs();
        for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
            unsigned r, q = rdiv(x, t[k].d, &r), rc;
            unsigned qc = rdiv(x & 0xff, t[k].d, &rc);
            if (t[k].q(x) != q || t[k].r(x) != r) bad |= 1;
            if (t[k].c((unsigned char)x) != (unsigned char)qc) bad |= 2;
        }
    }
    say(bad & 1 ? "DIV16 BAD q" : "DIV16 OK");
    say(bad & 2 ? " c-bad\n" : "\n");
    say("==END==\n");
    for (;;) ;
}
