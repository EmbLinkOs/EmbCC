/* A float load whose base address sits in a stack slot is addressed
 * through rcx; the code fetched that address through rax first, behind
 * the back of x86-64's rax residency cache, which still named what rax
 * held before. The next load that wanted that value -- here a short from
 * a local array whose address was cached in rax -- read from the float's
 * base instead. Found by the fuzzer (seed 1038932645, x86-64 at -O2, -O3
 * and -Os); this is the program reduced, and returns 42 when its checksum
 * is the one clang -O0 computes. */
// expect-exit: 42
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
static u16 gv0;
static u32 gv1[4];
static i8 gv2;
static struct SA gs;
__attribute__((noinline)) static i16 f0(void)
{
    u32 l0 = ((u32)1446645599U);
    f64 l1 = ((f64)-0.0);
    i16 la[4];
    for (u32 z = 0; z < 4; z++) la[z] = (i16)(z * ((u32)15U) + ((u32)31U));
    struct SB ls0;
    ls0.x = ((u64)4966546623294663216ULL);
    ls0.y = ((f32)0.5f);
    ls0.z = ((u8)164U);
    ls0.w = ((i32)2147483645);
    if (((u8)l0) <= ((u32)((u32)((u32)(((u32)gs.d) | ((u32)gv2))) >> (((u32)f2i_32(((f64)l0))) & 31)))) {
        for (u32 i1 = 0; i1 < (u32)(((((f64)(((f64)3.0) * ((f64)gs.a))) >= ((u16)((u32)((u16)gv0) + (u32)((u16)f2i_32(l1))))) ? ((u32)f2i_32(l1)) : ((u32)((u32)((u32)la[32U & 3]) * (u32)((u32)la[(u32)l0 & 3])))) & 15); i1++) {
            switch ((u32)((u32)3U) & 7) {
            }
        }
    }
    switch ((u32)((u32)f2i_32(((f64)(((f64)1e10) - ((f64)l1))))) & 7) {
    case 0:
        for (i32 i1 = 0; i1 < (i32)11; i1++) {
            switch ((u32)((u32)((u32)((u32)i1) * (u32)((((f64)((f32)-0.25f)) > ((u64)((u64)((u64)gv1[(u32)l0 & 3]) / ((u64)((u64)l0) | 1)))) ? ((u32)(((u32)i1) ^ ((u32)f2i_32(l1)))) : ((u32)gv0)))) & 3) {
            case 0:
                gv2 = ((i8)f2i_32(((f32)(((f32)(((f32)10.0f) * ((f32)(-((f32)la[(u32)i1 & 3]))))) + ((f32)((f64)(((f64)l0) - ((f64)la[(u32)i1 & 3]))))))));
                l0 = ((u32)gv2);
            case 2:
                l1 = ((((u64)f2i_64(((f64)(((((i64)f2i_64(gs.c)) > ((u32)f2i_32(l1))) ? ((f64)la[57U & 3]) : ((f64)0.1)) * ((f64)gv2))))) > ((u32)la[(u32)gv0 & 3])) ? ((f64)gv1[(u32)l0 & 3]) : ((f64)((i64)(((i8)(((i8)l0) & ((i8)l0))) <= ((f32)(((f32)l0) + ((f32)la[(u32)gv0 & 3])))))));
                gv2 = ((((u8)((u32)((u8)(((u8)f2i_32(l1)) ^ ((u8)((u32)((u8)31U) * (u32)((u8)127U))))) * (u32)((u8)la[29U & 3]))) == ((u32)gs.b)) ? ((i8)((u32)((i8)6) / ((u32)((((i8)(-(u32)((i8)3))) >= ((i32)((u32)((i32)7) - (u32)((i32)gv1[(u32)i1 & 3])))) ? ((i8)(((u8)i1) != ((i8)gs.d))) : ((i8)((u32)((i8)gs.b) + (u32)((i8)l0)))) | 1))) : ((i8)(~(u32)((i8)((u32)((i8)f2i_32(((f32)gs.c))) + (u32)((i8)((u32)((i8)l0) << (((u32)gv1[(u32)gv0 & 3]) & 31))))))));
            }
        }
        { i16 wb1[6];
        }
        if (((u16)((u32)((u16)la[(u32)gv0 & 3]) >> (((u32)((u32)((u32)gv0) - (u32)((u32)2784294141U))) & 31))) < ((i16)(!(u32)((i16)17596)))) {
            for (i64 i2 = 0; i2 < (i64)(((u32)f2i_32(((f32)l0))) & 15); i2++) {
            }
            for (i64 i2 = 0; i2 < (i64)(((u32)(((u32)(((u32)f2i_32(l1)) & ((u32)1918850433U))) ^ ((u32)((u32)((u32)la[(u32)gv0 & 3]) + (u32)((u32)gs.d))))) & 15); i2++) {
                if (((i8)((u32)((i8)(((u16)24505U) < ((i8)gv0))) % ((u32)((i8)f2i_32(((f32)gv2))) | 1))) <= ((f32)gs.c)) {
                }
            }
            switch ((u32)((u32)((u32)((u32)gv1[(u32)l0 & 3]) + (u32)((u32)f2i_32(l1)))) & 3) {
            }
            { u32 *pp2 = &gv1[(u32)l0 & 2];
              mix((u64)pp2[0] ^ (u64)pp2[1]); }
        }
    }
    return ((i16)gs.b);
}
__attribute__((noinline)) static i64 f1(f32 a0)
{
    u32 l0 = ((u32)2147483646U);
    i16 l1 = ((i16)3);
    u32 l2 = ((u32)2U);
    u64 la[16];
    for (u32 z = 0; z < 16; z++) la[z] = (u64)(z * ((u32)3840406260U) + ((u32)1U));
    mix_i((u64)f0());
    { u32 wb0[6];
      i64 wi0 = 0;
      mix((u64)wi0); }
    return ((i64)la[(u32)gv0 & 15]);
}
__attribute__((noinline)) static u32 f2(i8 a0)
{
    f64 l0 = ((f64)-0.0);
    f32 l1 = ((f32)1e10f);
    return ((u32)((u32)((((i16)18807) == ((f32)(((f32)(((f32)4294967296.0f) * ((f32)l0))) * ((f32)1e30f)))) ? ((u32)gs.d) : ((u32)f2i_32(l0))) / ((u32)((u32)((u32)((u32)f2i_32(gs.c)) - (u32)((u32)((u32)((u32)(!(u32)((u32)1911048006U))) / ((u32)((u32)gv1[(u32)gv2 & 3]) | 1))))) | 1)));
}
__attribute__((noinline)) static i32 f3(i64 a0, u64 a1)
{
    u64 l0 = ((u64)5966437217371649770ULL);
    if (((i64)(((i64)(((u8)0U) < ((u16)gv2))) ^ ((i64)(((f32)10.0f) < ((u64)a1))))) != ((f32)(((f32)gs.a) - ((f32)(((f32)4294967296.0f) - ((f32)2.5e-5f)))))) {
        if (((i8)gv2) != ((f64)gs.a)) {
        }
        switch ((u32)((u32)((u32)((u32)(((f32)((i16)gv1[(u32)gv0 & 3])) >= ((u8)((u32)((u8)gv2) + (u32)((u8)gs.a))))) - (u32)((u32)(((i8)(((u64)8878691685228100292ULL) <= ((i8)gv1[(u32)l0 & 3]))) == ((f32)(((f32)a0) / nz_f32(((f32)gv1[(u32)l0 & 3])))))))) & 3) {
            if (((f64)(-((f64)gv0))) >= ((i16)((u32)((i16)(((i32)l0) >= ((i32)l0))) % ((u32)((i16)((u32)((i16)gv1[(u32)l0 & 3]) << (((u32)gv2) & 31))) | 1)))) {
            }
            if (((f64)(((f64)(((f64)l0) + ((f64)gv1[(u32)gv0 & 3]))) + ((((i32)gv1[(u32)gv2 & 3]) > ((u16)l0)) ? ((f64)gv2) : ((f64)-0.0)))) == ((u8)(((((u16)gv0) <= ((f64)gv1[62U & 3])) ? ((u8)l0) : ((u8)a1)) & ((u8)((u32)((u8)l0) % ((u32)((u8)gs.a) | 1)))))) {
            }
        }
    }
    if (((f64)(((f64)(((f64)gs.a) / nz_f64(((f64)a0)))) + ((f64)(((f64)gs.b) * ((f64)gv0))))) > ((f32)((u64)f2i_64(((f32)gv0))))) {
        for (i64 i1 = 0; i1 < (i64)19; i1++) {
            for (i64 i2 = 0; i2 < (i64)(((u32)gv2) & 15); i2++) {
                gs.b = ((u32)((u32)((u32)f2i_32(((f64)gs.c))) * (u32)((u32)((u32)((u32)((u32)((u32)gs.b) / ((u32)((u32)(((u32)1146585984U) | ((u32)gv1[20U & 3]))) | 1))) >> (((u32)(~(u32)((u32)(((u64)f2i_64(gs.c)) > ((i64)a1))))) & 31)))));
            }
        }
    }
    return ((((u8)31U) != ((i8)((u32)((i8)((u32)((i8)gv1[36U & 3]) % ((u32)((i8)2) | 1))) + (u32)((i8)f2i_32(((((u8)gv0) > ((u16)a1)) ? ((f32)4294967296.0f) : ((f32)a1))))))) ? ((i32)(!(u32)((i32)gv1[(u32)l0 & 3]))) : ((i32)((u32)((i32)((u32)((i32)((u32)((i32)f2i_32(gs.c)) % ((u32)((i32)gs.d) | 1))) * (u32)((((i8)a1) != ((u32)gv1[(u32)a1 & 3])) ? ((i32)7) : ((i32)gs.b)))) - (u32)((((i64)gv1[(u32)a1 & 3]) != ((i32)((u32)((i32)a0) * (u32)((i32)a1)))) ? ((((i8)16) > ((u32)gv1[2U & 3])) ? ((i32)gs.b) : ((i32)l0)) : ((i32)((u32)((i32)gs.d) + (u32)((i32)gv2)))))));
}
__attribute__((noinline)) static struct SA g4(struct SC sp, i32 ka)
{
    i32 l0 = ((i32)(-172436940));
    f64 l1 = ((f64)-1.0);
    mix_i((u64)f1(((f32)(((f32)(-((f32)gv1[12U & 3]))) - ((f32)(((f32)gs.b) / nz_f32(((f32)gv1[(u32)ka & 3]))))))));
    struct SA ret;
    ret.a = ((i8)(!(u32)((i8)((u32)((i8)ka) - (u32)((i8)gv1[(u32)l0 & 3])))));
    ret.b = ((u32)(((((u32)l0) != ((i64)(((i64)(((i64)gv0) | ((i64)l0))) ^ ((i64)gs.b)))) ? ((u32)f2i_32(((f64)(((f64)gv1[(u32)l0 & 3]) * ((f64)-1e10))))) : ((u32)gv1[(u32)gv2 & 3])) & ((u32)(((u32)l0) > ((u32)((u32)((((u64)sp.p) > ((f32)-0.0f)) ? ((u32)2147483647U) : ((u32)gv0)) * (u32)((u32)(((u32)ka) ^ ((u32)gs.b)))))))));
    ret.c = ((f64)-1e10);
    ret.d = ((i16)f2i_32(((f64)(((f64)((u64)(~(u64)((u64)f2i_64(l1))))) * ((f64)((u64)f2i_64(((f32)-1e10f))))))));
    return ret;
}
__attribute__((noinline)) static u64 f5(i64 a0, u32 a1)
{
    f32 l0 = ((f32)1e30f);
    f32 l1 = ((f32)-0.0f);
    i32 l2 = ((i32)(-1835657970));
    struct SBF ls0;
    ls0.b3 = 0;
    ls0.s5 = 0;
    ls0.b12 = 0;
    ls0.s7 = 0;
    switch ((u32)((u32)(((u16)((u32)((u16)((u32)((u16)gs.b) + (u32)((u16)gv2))) - (u32)((u16)(~(u32)((u16)gs.d))))) <= ((u64)(((f32)(((f32)10.0f) * ((f32)gv1[11U & 3]))) < ((f32)(((f32)l0) / nz_f32(((f32)gs.c)))))))) & 7) {
        { u32 wb1[6];
          i32 wi1 = 0;
          mix((u64)wi1); }
        for (i64 i1 = 0; i1 < (i64)22; i1++) {
            for (i32 i2 = 0; i2 < (i32)(((u32)((u32)((u32)(!(u32)((u32)ls0.b12))) / ((u32)((u32)(((u16)f2i_32(l0)) == ((u32)3553601453U))) | 1))) & 15); i2++) {
            }
        }
        for (i64 i1 = 0; i1 < (i64)15; i1++) {
            if (((i64)gs.d) < ((((u16)(~(u32)((u16)ls0.s5))) == ((u32)(~(u32)((u32)i1)))) ? ((u64)a1) : ((u64)gv1[(u32)i1 & 3]))) {
            }
        }
    }
    { u32 wb0[6];
      u8 wi0 = 0;
      mix((u64)wi0); }
    return ((u64)gv1[(u32)gv2 & 3]);
}
int main(void)
{
    mix_i((u64)f3((i64)(((i64)6143599113133215624LL) + vzero), (u64)(((u64)8680404977780157993ULL) + vzero)));
    { struct SC in = { 0 }; struct SA o;
      o = g4(in, (i32)(5 + vzero));
    mix((u64)o.a);     mix((u64)o.b);     mix_f(o.c);     mix((u64)o.d);  }
    mix_i((u64)gv2);
    return ck == 11405550540210774631ULL ? 42 : 1;
}
