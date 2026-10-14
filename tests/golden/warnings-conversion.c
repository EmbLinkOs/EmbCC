/* -Wconversion, -Wsign-conversion, -Wfloat-conversion and -Woverflow, by
 * line: W: names the warnings a line gets (in either order), a line
 * without one gets none. GCC 16 gives exactly these, except where a line
 * says gcc-folds: GCC simplifies the expression first (`(u >> 24) & 0xff`
 * to `u >> 24`) and then warns about what it made; the value fits, and
 * EmbCC says nothing. warnings-conversion.sh compiles this for x86-64. */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef signed char s8; typedef short s16;
typedef long long s64; typedef unsigned long long u64;
extern int i; extern unsigned u; extern u8 b; extern s8 sc; extern u16 h;
extern s16 sh; extern long l; extern unsigned long ul; extern float f;
extern double d; extern _Bool bo; extern char ch;
enum e { E0, E1 }; extern enum e ev;
enum n { N0 = -1 }; extern enum n nv;
struct bf { unsigned a : 3; int s : 4; unsigned one : 1; u8 c : 2; };
extern struct bf bs;
void take8(u8); void takeu(unsigned); void takei(int); void takef(float);
void takeul(unsigned long);

u8 r1(void) { return i; }                       /* W:conversion */
u8 r2(void) { return i & 0xff; }                /* W:conversion */
u8 r3(void) { return b + 1; }
u8 r4(void) { return (u8)(b + 1); }

void t(void)
{
    u8 x = 0; int k = 0; unsigned v = 0;
    x = i;                                      /* W:conversion */
    x = 300;                                    /* W:overflow */
    x = 255;
    x = -1;                                     /* W:sign-conversion */
    x = b + b;
    x = b & i;                                  /* W:conversion */
    x = i % 16;                                 /* W:conversion */
    x = u % 16;
    x = u / 16;                                 /* W:conversion */
    x = (u >> 24) & 0xff;                       /* gcc-folds */
    x = i & 0x7f;
    x = b ^ i;                                  /* W:conversion */
    x += 1;
    x += i;                                     /* W:conversion */
    x++;
    x = ch;                                     /* W:sign-conversion */
    x = sc;                                     /* W:sign-conversion */
    x = bo;
    x = i ? 1 : 2;
    x = sc < 0 ? 0 : sc;                        /* W:sign-conversion */
    x = b << 1;
    x = ~b;
    x = -b;
    x = ~0;                                     /* W:sign-conversion */
    x = 1 << 8;                                 /* W:overflow */
    x = 'a' + 1;
    x = (u8)i | b;
    v = i;                                      /* W:sign-conversion */
    v = -1;                                     /* W:sign-conversion */
    v = i + u;                                  /* W:sign-conversion */
    v = u + 1;
    v = u + -1;                                 /* W:sign-conversion */
    v = sizeof(int);
    v = l;                                      /* W:conversion */
    v = ul;                                     /* W:conversion */
    v = (i & 0xff);
    v = i & 0xffu;                              /* W:sign-conversion */
    v = b;
    v = sc;                                     /* W:sign-conversion */
    v = 1 << 31;                                /* W:sign-conversion */
    v += i;                                     /* W:sign-conversion */
    v -= 1;
    v = b + -1;
    ul = (unsigned long)i;
    takeul((unsigned long)i);
    takeu(0x70 + (i ? 13 : 31));
    takei((unsigned)(u & 0xff));
    takeu((i ? 1 : 2) * 4);
    k = u;                                      /* W:sign-conversion */
    k = l;                                      /* W:conversion */
    k = f;                                      /* W:float-conversion */
    k = d;                                      /* W:float-conversion */
    k = 3.0;
    k = 3.5;                                    /* W:float-conversion */
    k = i + u;                                  /* W:sign-conversion,sign-conversion */
    k += u;                                     /* W:sign-conversion,sign-conversion */
    k = -u;                                     /* W:sign-conversion */
    k = 0x80000000;                             /* W:sign-conversion */
    k = u > 3;
    k = !u;
    if (i < u) k = 0;
    f = d;                                      /* W:float-conversion */
    f = i;                                      /* W:conversion */
    f = b;
    f = 1.5;
    f = 0.1;                                    /* W:float-conversion */
    f = f * 2.0;                                /* W:float-conversion */
    f = f * 2.0f;
    f = i + f;                                  /* W:conversion */
    f = 16777217;                               /* W:float-conversion */
    f = 16777216;
    d = i;
    d = l;                                      /* W:conversion */
    take8(i);                                   /* W:conversion */
    takeu(i);                                   /* W:sign-conversion */
    takei(u);                                   /* W:sign-conversion */
    takef(d);                                   /* W:float-conversion */
    ul = i;                                     /* W:sign-conversion */
    l = u;
    ch = i;                                     /* W:conversion */
    ch = b;                                     /* W:sign-conversion */
    x = ev;
    ev = i;
    v = nv;
    bs.a = i;                                   /* W:conversion */
    bs.a = 9;                                   /* W:overflow */
    bs.a = 7;
    bs.s = 8;                                   /* W:conversion */
    bs.s = 7;
    bs.one = i > 0;
    bs.c = b;                                   /* W:conversion */
    bs.a = u & 7;
    x = bs.a;
    v = bs.s;                                   /* W:sign-conversion */
    u64 y = i;                                  /* W:sign-conversion */
    s64 z = ul;                                 /* W:sign-conversion */
    s16 q = h;                                  /* W:sign-conversion */
    u16 w = h + h;
    struct bf ib = { 9, 0, 0, 0 };              /* W:overflow */
    (void)x; (void)k; (void)v; (void)y; (void)z; (void)q; (void)w; (void)ib;
}
