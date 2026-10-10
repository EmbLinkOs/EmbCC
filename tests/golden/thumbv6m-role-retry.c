/* verify/fuzz seed 10029 (gen2), reduced: f0 has a compare lowered while
 * only one scratch role could be had. The attempt is thrown away and the
 * function lowered again with other roles, as the backend is designed to;
 * the compare raised an internal error first ("a compare with no register
 * for its result"). thumbv6m-target.sh compiles it at -O1, -O2 and -Os. */
typedef signed char i8; typedef unsigned char u8;
typedef short i16; typedef unsigned short u16;
typedef int i32; typedef unsigned int u32;
typedef long long i64; typedef unsigned long long u64;
typedef float f32; typedef double f64;
u64 ck = 1469598103934665603ULL;
static void mix(u64 v) { ck = (ck ^ v) * 1099511628211ULL; ck ^= ck >> 29; }
static void mix_i(u64 v) { mix(v); }
static void mix_f(f64 d) { union { f64 d; u64 u; } x; if (d != d) { mix(0x7ff8000000000000ULL); return; } x.d = d; mix(x.u); }  /* one NaN */
static i32 f2i_32(f64 d) { return (d > -2147483648.0 && d < 2147483647.0) ? (i32)d : 0; }
static i64 f2i_64(f64 d) { return (d > -9.2e18 && d < 9.2e18) ? (i64)d : 0; }
static f32 nz_f32(f32 v) { return (v > -1e-20f && v < 1e-20f) ? 1.5f : v; }
static f64 nz_f64(f64 v) { return (v > -1e-200 && v < 1e-200) ? 1.5 : v; }
static volatile u32 vzero = 0;
struct SA { i8 a; u32 b; f64 c; i16 d; };
struct SB { u64 x; f32 y; u8 z; i32 w; };
struct SC { u16 p; u16 q; };
struct SBF { unsigned b3 : 3; signed s5 : 5; unsigned b12 : 12; signed s7 : 7; };
static u8 gv0;
static struct SA gs;
__attribute__((noinline)) static u64 f0(u64 a0)
{
    f64 l0 = ((f64)1e-300); /*K*/
    struct SC ls0; /*K*/
    ls0.p = ((u16)50244U); /*K*/
    ls0.q = ((u16)7032U); /*K*/
    struct SC ls1; /*K*/
    ls1.p = ((u16)100U); /*K*/
    ls1.q = ((u16)32765U); /*K*/
    l0 = ((f64)(((f64)((i8)((u32)((i8)44) >> (((u32)(((u32)3584898726U) ^ ((u32)613793933U))) & 31)))) + ((f64)((f32)gs.d))));
    if (((i32)((u32)((i32)((u32)((i32)gs.b) / ((u32)((i32)ls1.p) | 1))) << (((u32)f2i_32(gs.c)) & 31))) == ((f64)ls0.p)) {
        ls1.p = ((u16)gv0);
    }
    gs.b = ((u32)ls1.q);
    if (((u32)f2i_32(((f32)(((f32)gv0) * ((f32)ls0.p))))) > ((f64)-7.125)) {
        for (u32 i1 = 0; i1 < (u32)(((u32)127U) & 15); i1++) {
            l0 = ((((i16)((u32)((i16)(-9169)) - (u32)((((f32)(((f32)i1) / nz_f32(((f32)ls1.q)))) != ((((i32)a0) != ((u8)ls0.q)) ? ((u8)i1) : ((u8)i1))) ? ((i16)ls0.q) : ((i16)30593)))) > ((f64)(((f64)gs.b) - ((f64)((u16)f2i_32(((f64)gs.d))))))) ? ((f64)(((f64)-0.25) - ((f64)ls1.p))) : ((f64)(((f64)(((f64)(((f64)ls1.p) - ((f64)ls1.q))) + ((f64)(((f64)gs.b) * ((f64)gs.d))))) * ((f64)gs.b))));
            gv0 = ((u8)(((u8)(-(u32)((u8)((u32)((u8)f2i_32(((f64)0.5))) * (u32)((u8)ls1.p))))) ^ ((u8)(((i8)(((i8)((u32)((i8)ls0.q) / ((u32)((i8)126) | 1))) & ((i8)f2i_32(((f64)a0))))) != ((u16)a0)))));
        }
    } else {
        if (((i16)((u32)((i16)ls0.p) / ((u32)((i16)((u32)((i16)31) * (u32)((i16)14003))) | 1))) == ((u64)((u64)((u64)((u64)((u64)f2i_64(gs.c)) + (u64)((u64)a0))) - (u64)((u64)f2i_64(((f32)ls1.p)))))) {
            if (((((i32)((u32)((i32)f2i_32(l0)) << (((u32)f2i_32(l0)) & 31))) != ((f32)ls1.p)) ? ((f32)(((f32)gs.d) + ((f32)a0))) : ((f32)(((f32)ls0.p) / nz_f32(((f32)2.5e-5f))))) != ((u16)((u32)((u16)f2i_32(((f32)gs.b))) << (((u32)f2i_32(((f32)3.0f))) & 31)))) {
            }
        } else {
            if (((u8)f2i_32(((f32)(((f32)l0) / nz_f32(((f32)gv0)))))) < ((f32)(((f32)gs.b) - ((f32)(-((f32)123.456f)))))) {
            }
            mix_i((u64)((((i8)a0) == ((u64)((u64)((u64)ls0.p) + (u64)((u64)gv0)))) ? ((u8)((u32)((u8)ls0.p) >> (((u32)gv0) & 31))) : ((u8)ls0.q)));
        }
    }
    return ((u64)((u64)((((i8)((u32)((i8)((u32)((i8)ls0.p) >> (((u32)gs.a) & 31))) * (u32)((i8)((u32)((i8)f2i_32(l0)) + (u32)((i8)f2i_32(l0)))))) < ((u32)gs.b)) ? ((((((i16)f2i_32(l0)) != ((u64)a0)) ? ((u32)ls1.q) : ((u32)7U)) < ((i32)a0)) ? ((u64)((u64)((u64)gv0) / ((u64)((u64)a0) | 1))) : ((((i64)f2i_64(l0)) == ((f64)l0)) ? ((u64)3ULL) : ((u64)gs.a))) : ((u64)ls0.p)) / ((u64)((u64)((u64)((u64)((u64)((((u8)f2i_32(l0)) == ((u32)gv0)) ? ((u64)gv0) : ((u64)f2i_64(l0))) << (((u32)f2i_32(((f32)l0))) & 63))) * (u64)((u64)9223372036854775805ULL))) | 1)));
}
__attribute__((noinline)) static u8 f1(void)
{
    f32 l0 = ((f32)0.5f); /*K*/
    i64 l1 = ((i64)3329957102138809484LL); /*K*/
    if (((u64)((u64)((u64)f2i_64(gs.c)) - (u64)((u64)9223372036854775807ULL))) < ((u32)((u32)((u32)((u32)((u32)1374723617U) >> (((u32)l1) & 31))) * (u32)((u32)15U)))) {
        switch ((u32)((u32)((u32)((u32)gs.b) - (u32)((((u16)(-(u32)((u16)gs.b))) > ((u16)f2i_32(((f64)gs.a)))) ? ((u32)l1) : ((u32)gs.d)))) & 7) {
        }
        switch ((u32)((u32)gs.b) & 7) {
            if (((i16)((u32)((i16)32677) * (u32)((i16)((u32)((i16)f2i_32(gs.c)) / ((u32)((i16)gv0) | 1))))) >= ((i32)(!(u32)((i32)f2i_32(l0))))) {
            }
        }
    }
    return ((u8)31U);
}
__attribute__((noinline)) static f64 f2(i32 a0)
{
    u16 l0 = ((u16)3U); /*K*/
    i16 l1 = ((i16)7); /*K*/
    f64 l2 = ((f64)1.0); /*K*/
    i16 la[16]; /*K*/
    for (u32 z = 0; z < 16; z++) la[z] = (i16)(z * ((u32)4244372479U) + ((u32)255U)); /*K*/
    { i16 wb0[18];
      i64 wi0 = 0;
      mix((u64)wi0); }
    return ((f64)(((f64)a0) / nz_f64(((f64)(((f64)1e30) * ((f64)gs.c))))));
}
__attribute__((noinline)) static f64 f3(void)
{
    f32 l0 = ((f32)65536.0f); /*K*/
    u16 l1 = ((u16)255U); /*K*/
    f64 l2 = ((f64)1.0); /*K*/
    f32 l3 = ((f32)65536.0f); /*K*/
    f32 la[8]; /*K*/
    for (u32 z = 0; z < 8; z++) la[z] = (f32)(z * ((u32)2147483645U) + ((u32)0U)); /*K*/
    if (((u16)32765U) == ((f64)((f32)4294967296.0f))) {
        { f32 *pp1 = &la[(u32)l1 & 6];
          mix_f(pp1[0] + pp1[1]); }
        for (i64 i1 = 0; i1 < (i64)(((u32)gs.b) & 15); i1++) {
        }
    }
    return ((f64)(-((f64)(((f64)(((f64)((f32)l1)) / nz_f64(((f64)gv0)))) + ((f64)gs.d)))));
}
__attribute__((noinline)) static struct SA g4(struct SC sp, i32 ka)
{
    u32 l0 = ((u32)426738080U); /*K*/
    f32 l1 = ((f32)4294967296.0f); /*K*/
    u64 l2 = ((u64)127ULL); /*K*/
    f64 la[4]; /*K*/
    for (u32 z = 0; z < 4; z++) la[z] = (f64)(z * ((u32)3128079031U) + ((u32)2147483647U)); /*K*/
    struct SBF ls0; /*K*/
    ls0.b3 = 0; /*K*/
    ls0.s5 = 0; /*K*/
    ls0.b12 = 0; /*K*/
    ls0.s7 = 0; /*K*/
    struct SC ls1; /*K*/
    ls1.p = ((u16)16U); /*K*/
    ls1.q = ((u16)15225U); /*K*/
    struct SA ret; /*K*/
    ret.a = ((i8)((u32)((i8)(((i8)((u32)((i8)l0) - (u32)((((i16)(-17916)) != ((i8)115)) ? ((i8)gv0) : ((i8)l2)))) | ((((i8)((u32)((i8)gv0) / ((u32)((i8)ls0.s5) | 1))) >= ((u32)(((f32)ls1.p) >= ((i64)l2)))) ? ((i8)5) : ((i8)f2i_32(gs.c))))) - (u32)((i8)f2i_32(l1)))); /*K*/
    ret.b = ((u32)f2i_32(la[(u32)gv0 & 3])); /*K*/
    ret.c = ((f64)(((f64)0.1) / nz_f64(((f64)(((f64)(((((i32)l0) > ((i64)f2i_64(l1))) ? ((f64)sp.q) : ((f64)sp.p)) + ((f64)(((f64)la[(u32)gv0 & 3]) * ((f64)la[(u32)ka & 3]))))) * ((f64)(((f64)((f32)la[(u32)ka & 3])) + ((f64)(((f64)3.0) / nz_f64(((f64)la[(u32)l0 & 3]))))))))))); /*K*/
    ret.d = ((i16)f2i_32(la[(u32)l0 & 3])); /*K*/
    return ret;
}
__attribute__((noinline)) static struct SC g5(struct SA sp, i32 ka)
{
    u16 l0 = ((u16)32767U); /*K*/
    u64 l1 = ((u64)14692126839684531036ULL); /*K*/
    if (((u8)((u32)((u8)((u32)((u8)ka) - (u32)((u8)45U))) - (u32)((u8)l0))) <= ((f64)l1)) {
    }
    if (((u64)((u64)((u64)gs.b) + (u64)((u64)((u64)((u64)100ULL) + (u64)((u64)gv0))))) < ((((u16)f2i_32(((f32)gv0))) != ((f64)(-((f64)gs.a)))) ? ((u8)sp.b) : ((u8)ka))) {
        if (((f32)(-((f32)gv0))) == ((i8)((u32)((i8)(!(u32)((i8)gv0))) % ((u32)((i8)((u32)((i8)ka) % ((u32)((i8)gs.a) | 1))) | 1)))) {
        }
        for (i64 i1 = 0; i1 < (i64)(((u32)gv0) & 15); i1++) {
        }
        switch ((u32)((u32)((u32)((u32)2651111212U) + (u32)((u32)((u32)((u32)gs.d) - (u32)((u32)l0))))) & 3) {
            switch ((u32)((u32)((u32)((u32)f2i_32(((f64)gv0))) + (u32)((u32)gv0))) & 7) {
            }
        }
    }
    struct SC ret; /*K*/
    ret.p = ((u16)((u32)((u16)sp.a) >> (((u32)f2i_32(((f64)(((f64)(-((f64)1e30))) / nz_f64(((f64)((f32)ka))))))) & 31))); /*K*/
    ret.q = ((u16)l0); /*K*/
    return ret;
}
__attribute__((noinline)) static f32 f6(u16 a0, f64 a1, i16 a2, u32 a3)
{
    f64 l0 = ((f64)0.0); /*K*/
    i16 la[8]; /*K*/
    for (u32 z = 0; z < 8; z++) la[z] = (i16)(z * ((u32)31U) + ((u32)100U)); /*K*/
    struct SB ls0; /*K*/
    ls0.x = ((u64)9223372036854775805ULL); /*K*/
    ls0.y = ((f32)4294967296.0f); /*K*/
    ls0.z = ((u8)0U); /*K*/
    ls0.w = ((i32)63); /*K*/
    struct SC ls1; /*K*/
    ls1.p = ((u16)2U); /*K*/
    ls1.q = ((u16)40018U); /*K*/
    if (((u32)f2i_32(((f32)la[(u32)gv0 & 7]))) >= ((i16)((u32)((i16)f2i_32(a1)) / ((u32)((i16)((u32)((i16)la[(u32)a2 & 7]) * (u32)((i16)63))) | 1)))) {
        if (((u8)((u32)((u8)a0) / ((u32)((u8)f2i_32(a1)) | 1))) < ((u16)(((u16)(((u16)ls1.p) & ((u16)ls1.p))) & ((u16)(((f32)la[(u32)a2 & 7]) <= ((f32)a3)))))) {
            if (((u32)(~(u32)((u32)f2i_32(((f64)gs.a))))) >= ((f32)(-((f32)gv0)))) {
            }
            mix_i((u64)f0(((u64)(~(u64)((u64)a2)))));
        }
        for (i64 i1 = 0; i1 < (i64)10; i1++) {
            switch ((u32)((u32)(((i16)(((i16)f2i_32(((f32)l0))) & ((i16)((u32)((i16)f2i_32(a1)) + (u32)((i16)255))))) >= ((f32)la[18U & 7]))) & 3) {
            }
        }
    }
    return ((f32)((u32)((u32)((u32)((u32)((u32)f2i_32(((f32)ls1.q))) * (u32)((u32)(((u32)a0) & ((u32)ls0.w))))) * (u32)((u32)((u32)((((f64)la[(u32)a2 & 7]) != ((i16)a0)) ? ((u32)gs.d) : ((u32)gv0)) * (u32)((u32)a2))))));
}
int main(void)
{
    mix_f(f6((u16)(((u16)32765U) + vzero), (f64)(((f64)123.456) + vzero), (i16)(((i16)29435) + vzero), (u32)(((u32)3212653127U) + vzero)));
    { struct SA in = { 0 }; struct SC o;
    mix((u64)o.p);     mix((u64)o.q);  }
}
