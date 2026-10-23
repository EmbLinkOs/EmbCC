/* Loads and stores more than 4 KB from their base, of every width,
 * forwards and backwards: what no Thumb-2 immediate reaches (12 bits up,
 * 8 down; ARMv6-M's reach is 5 bits times the size), so the address has
 * to be made.
 *
 * GCC's torture suite found the ARMv7-M backend stopping on a struct of
 * more than 4 KB passed by value ("internal error: thumb: a 4-byte load
 * at offset 4096 from r2 is out of reach": execute/pr20621-1.c,
 * pr23135.c, pr28982b.c, compile/20050622-1.c): its words were copied to
 * the outgoing area one by one in straight-line code, at offsets no
 * immediate reaches -- and the 72 MB struct of 20050622-1.c, on ARMv6-M,
 * was a compile that did not finish. Now a loop copies them. The
 * arguments after such a struct are as far from the sp, in the caller
 * (take's x, y and z) and in the callee. The rest are the other ways an
 * offset gets that big: a struct's members, a pointer's negative index,
 * a frame past 4 KB. */
// expect-exit: 42

struct big { int i[0x420]; };           /* 4224 bytes on a 32-bit target */
struct mid { char c[4200]; int tail; };
struct odd { char c[4199]; };           /* the loop's tail: three bytes */
struct big gb;
struct pad {
    char pre[4100];
    signed char sc; unsigned char uc; short ss; unsigned short us;
    int si; long long ll; float f; double d;
    unsigned bf : 5; unsigned bf2 : 11;
} gp;
int arr[1200];

__attribute__((noinline)) int take(struct big b, int x, long long y, int z)
{
    return b.i[0] + b.i[0x41f] + x + (int)y + z;
}

__attribute__((noinline)) int take_mid(struct mid m, int x)
{
    return m.c[4199] + m.tail + x;
}

__attribute__((noinline)) int take_odd(int x, struct odd o, double d)
{
    return o.c[0] + o.c[4198] + x + (int)d;
}

__attribute__((noinline)) int fields(struct pad *p)
{
    p->sc = -3; p->uc = 200; p->ss = -300; p->us = 60000;
    p->si = -70000; p->ll = -5000000000LL; p->f = 1.5f; p->d = -2.25;
    p->bf = 17; p->bf2 = 1000;
    return p->sc + p->uc + p->ss + p->us + p->si + (int)(p->ll / 1000000)
           + (int)(p->f * 2) + (int)(p->d * 4) + p->bf + p->bf2;
}

/* q is 1200 ints into arr: every width, 4 KB and more behind it */
__attribute__((noinline)) int back(int *q)
{
    q[-1100] = 11;
    ((short *)q)[-2100] = 12;
    ((signed char *)q)[-4500] = -13;
    ((long long *)q)[-575] = 14;
    ((double *)q)[-600] = 15.0;
    return q[-1100] + ((short *)q)[-2100] + ((signed char *)q)[-4500]
           + (int)((long long *)q)[-575] + (int)((double *)q)[-600];
}

__attribute__((noinline)) int frame(int k)
{
    volatile int a[1100];
    volatile long long ll[4];
    volatile double d[4];
    volatile int x = k;
    a[0] = k; a[1099] = k + 1;
    ll[3] = k; d[3] = k;
    return a[0] + a[1099] + (int)ll[3] + (int)d[3] + x;
}

int main(void)
{
    struct mid m;
    static struct odd o;
    int r;
    gb.i[0] = 1; gb.i[0x41f] = 2;
    for (int i = 0; i < 4200; i++)
        m.c[i] = (char)i;
    m.c[4199] = 3; m.tail = 4;
    o.c[0] = 1; o.c[4198] = 2;
    r = take(gb, 5, 6, 7);                                      /* 21 */
    r += take_mid(m, 1);                                        /* 8 */
    r += take_odd(3, o, 4.0) == 10;
    r += fields(&gp) == -3 + 200 - 300 + 60000 - 70000 - 5000 + 3 - 9
                        + 17 + 1000;
    r += back(arr + 1200) == 11 + 12 - 13 + 14 + 15;
    r += frame(2) == 2 + 3 + 2 + 2 + 2;
    return r + 9;
}
