/* Fuzz seed 5023 (gen2), reduced. A call's arguments are out of line
 * (ir.h, argv), so copying an instruction shares them; switch threading
 * copied a block holding a call and renamed the COPY's operands, which
 * renamed the original's too: at -O2 the original call passed the copy's
 * values, on every target. The IR verifier (EMBCC_VERIFY, set by the
 * whole suite) refuses two calls sharing one array, so this fails to
 * compile without the fix (opt.c, ins_own_args) as well as running wrong.
 */
// expect-exit: 42
typedef signed char i8; typedef unsigned char u8;
typedef short i16; typedef unsigned short u16;
typedef int i32; typedef unsigned int u32;
typedef long long i64; typedef unsigned long long u64;
typedef float f32; typedef double f64;
u64 ck = 1469598103934665603ULL;
static void mix(u64 v) { ck = (ck ^ v) * 1099511628211ULL; ck ^= ck >> 29; }
static void mix_i(u64 v) { mix(v); }
static i32 f2i_32(f64 d) { return (d > -2147483648.0 && d < 2147483647.0) ? (i32)d : 0; }
static i64 f2i_64(f64 d) { return (d > -9.2e18 && d < 9.2e18) ? (i64)d : 0; }
static f32 nz_f32(f32 v) { return (v > -1e-20f && v < 1e-20f) ? 1.5f : v; }
static f64 nz_f64(f64 v) { return (v > -1e-200 && v < 1e-200) ? 1.5 : v; }
static volatile u32 vzero = 0;
struct SA { i8 a; u32 b; f64 c; i16 d; };
struct SC { u16 p; u16 q; };
struct SBF { unsigned b3 : 3; signed s5 : 5; unsigned b12 : 12; signed s7 : 7; };
static u32 gv0[16];
static i32 gv1[4];
static u64 gv2[16];
static struct SA gs;
__attribute__((noinline)) static i32 f0(u32 a0)
{
    u64 l0 = ((u64)16413126085721620555ULL); 
    i8 la[8]; 
    struct SBF ls0; 
    if (((f64)((i16)la[(u32)l0 & 7])) != ((u64)3ULL)) {
        switch ((u32)((u32)gs.d) & 3) {
            if (((u8)((u32)((u8)((u32)((u8)2U) >> (((u32)gs.a) & 31))) >> (((u32)63U) & 31))) < ((f32)((i16)(((i16)l0) & ((i16)l0))))) {
                if (((f64)(((f64)((i8)l0)) * ((f64)gv1[(u32)l0 & 3]))) < ((i32)((u32)((i32)((u32)((i32)gs.d) + (u32)((i32)l0))) >> (((u32)l0) & 31)))) {
                    if (((u64)l0) == ((u32)((u32)((u32)15U) / ((u32)((u32)f2i_32(gs.c)) | 1)))) {
                    }
                }
            }
            for (u8 i2 = 0; i2 < (u8)(((u32)((u32)((u32)((u32)((u32)gs.d) % ((u32)((u32)ls0.b12) | 1))) - (u32)((((i16)a0) >= ((i16)a0)) ? ((u32)gv0[(u32)a0 & 15]) : ((u32)a0)))) & 15); i2++) {
                if (((i16)((u32)((i16)(((i16)i2) | ((i16)gs.a))) - (u32)((i16)((u32)((i16)l0) + (u32)((i16)gs.b))))) < ((u8)i2)) {
                }
            }
            if (((u8)(((u8)(!(u32)((u8)a0))) | ((u8)ls0.b3))) > ((u32)a0)) {
            }
            if (((u16)((u32)((u16)la[(u32)a0 & 7]) - (u32)((u16)((u32)((u16)gs.b) + (u32)((u16)21459U))))) >= ((f32)(((f32)(((f32)123.456f) - ((f32)gs.c))) / nz_f32(((f32)(((f32)a0) - ((f32)a0))))))) {
            }
        }
    }
    return ((i32)(((i32)gv1[38U & 3]) | ((i32)2147483647)));
}
__attribute__((noinline)) static i8 f1(u32 a0)
{
    for (u8 i0 = 0; i0 < (u8)1; i0++) {
        { u32 wb1[18];
          u32 wi1 = 0;
          mix((u64)wi1); }
    }
    return ((i8)((u32)((i8)124) + (u32)((i8)63)));
}
__attribute__((noinline)) static f64 f2(f64 a0, f64 a1, i64 a2, u16 a3)
{
    f64 l0 = ((f64)65536.0); 
    f64 l2 = ((f64)1e10); 
    if (((i8)f2i_32(l0)) < ((u32)((u32)((u32)((u32)((u32)2147483647U) + (u32)((u32)f2i_32(l0)))) >> (((u32)f2i_32(((f64)65536.0))) & 31)))) {
        { u32 wb1[18];
            if (((i16)((u32)((i16)gv2[(u32)a2 & 15]) - (u32)((i16)(-(u32)((i16)a3))))) >= ((i16)(((i64)((u64)((i64)gv1[47U & 3]) % ((u64)((i64)a3) | 1))) >= ((u16)7U)))) {
            }
        }
    }
    return ((((f64)1.0) != ((i64)((u64)((i64)15LL) << (((u32)((u32)((u32)((u32)((u32)2U) % ((u32)((u32)f2i_32(a0)) | 1))) << (((u32)((u32)((u32)f2i_32(l2)) * (u32)((u32)f2i_32(a1)))) & 31))) & 63)))) ? ((f64)(((f64)l2) * ((f64)(((f64)((f32)a0)) / nz_f64(((f64)((f32)-1e10f))))))) : ((f64)(-((f64)((((u16)f2i_32(l2)) != ((u8)f2i_32(gs.c))) ? ((f32)a1) : ((f32)a1))))));
}
__attribute__((noinline)) static i8 f3(f32 a0, u32 a1, u8 a2)
{
    i8 l0 = ((i8)2); 
    i16 l1 = ((i16)255); 
    return ((i8)(((i8)(!(u32)((i8)l0))) & ((i8)((u32)((i8)(~(u32)((i8)l1))) / ((u32)((i8)((u32)((i8)gv1[61U & 3]) + (u32)((i8)((u32)((i8)l0) * (u32)((i8)f2i_32(a0)))))) | 1)))));
}
__attribute__((noinline)) static struct SC g4(struct SA sp, i32 ka)
{
    struct SC ret; 
    return ret;
}
__attribute__((noinline)) static i8 f5(i8 a0, u32 a1, u8 a2, i64 a3)
{
    f32 l0 = ((f32)10.0f); 
    f32 l2 = ((f32)1e-10f); 
    i8 l3 = ((i8)28); 
    for (i64 i0 = 0; i0 < (i64)(((u32)((u32)((u32)((u32)((u32)f2i_32(l2)) + (u32)((u32)f2i_32(l0)))) - (u32)((u32)((u32)((u32)a2) - (u32)((u32)2147483646U))))) & 15); i0++) {
        switch ((u32)((u32)l3) & 7) {
        case 0:
            if (((i32)((u32)((i32)(((i32)gv0[40U & 15]) | ((i32)1383242807))) * (u32)((i32)(!(u32)((i32)gv1[(u32)a0 & 3]))))) > ((u64)f2i_64(l2))) {
            }
        case 5:
        case 6:
            if (((i32)gv1[(u32)a2 & 3]) == ((i16)((u32)((i16)(((i16)a1) & ((i16)a1))) / ((u32)((i16)((u32)((i16)i0) / ((u32)((i16)a2) | 1))) | 1)))) {
            }
        case 7:
        }
    }
    return ((i8)a0);
}
int main(void)
{
    mix_i((u64)f3((f32)(((f32)1e-10f) + vzero), (u32)(((u32)1379991523U) + vzero), (u8)(((u8)225U) + vzero)));
    mix_i((u64)f5((i8)(((i8)127) + vzero), (u32)(((u32)3U) + vzero), (u8)(((u8)126U) + vzero), (i64)(((i64)9223372036854775805LL) + vzero)));
    return (int)(ck % 251) == 239 ? 42 : 1;
}
