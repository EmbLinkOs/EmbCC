/* Fuzz seed 927 (gen2), reduced: wrong at -O1 and up on every 32-bit
 * target (SPARC, RV32, Cortex-M, MIPS), right on x86-64 and AArch64.
 * `l0 = (u64)a0` widened a byte that was promoted to int, and the
 * known-zero rule made the widening an eight-byte COPY of the four-byte
 * value -- whose high word, where a 64-bit value is a register pair, is
 * whatever that register held (src/opt/opt.c, kz_copy_wide_ok). The
 * reduction keeps the shape the fold needed: the loop bound and l0 share
 * the extension of a0, and l0 reaches a double whole. */
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
static f64 nz_f64(f64 v) { return (v > -1e-200 && v < 1e-200) ? 1.5 : v; }
static volatile u32 vzero = 0;
struct SA { i8 a; u32 b; f64 c; i16 d; };
struct SC { u16 p; u16 q; };
struct SBF { unsigned b3 : 3; signed s5 : 5; unsigned b12 : 12; signed s7 : 7; };
static u8 gv0;
static f64 gv1[8];
static i8 gv2[4];
static struct SA gs;
__attribute__((noinline)) static i8 f1(u8 a0, u64 a1, f64 a2)
{
    u64 l0 = ((u64)0ULL); 
    struct SA ls1; 
    ls1.a = ((i8)127); ls1.c = 2.5e-5;
    for (i64 i0 = 0; i0 < (i64)5; i0++) {
        for (u8 i1 = 0; i1 < (u8)(((u32)gv2[(u32)l0 & 3]) & 15); i1++) {
            a0 = ((u8)f2i_32(((f64)(((f64)(((f64)1.0) * ((f64)gv1[(u32)a0 & 7]))) / nz_f64(((f64)(((f64)(((f64)a1) + ((f64)i0))) + ((f64)(((f64)gv2[(u32)gv0 & 3]) - ((f64)a0))))))))));
        }
        switch ((u32)((u32)f2i_32(ls1.c)) & 3) {
            for (i64 i2 = 0; i2 < (i64)15; i2++) {
            }
        }
        switch ((u32)((u32)a0) & 7) {
            { f64 *pp2 = &gv1[1U & 6];
            }
        }
    }
    for (i64 i0 = 0; i0 < (i64)(((u32)(-(u32)((u32)((u32)((u32)a0) + (u32)((u32)gs.b))))) & 15); i0++) {
        l0 = ((u64)a0);
        { i8 *pp1 = &gv2[16U & 2];
          mix((u64)pp1[0] ^ (u64)pp1[1]); }
    }
    return ((((i8)f2i_32(ls1.c)) > ((f64)(-((f64)(((f64)((i8)f2i_32(a2))) - ((f64)(((f64)l0) - ((f64)ls1.a)))))))) ? ((i8)gv0) : ((i8)(((u16)f2i_32(((f64)(((f64)gv2[(u32)a1 & 3]) * ((f64)a0))))) > ((u16)((u32)((u16)a1) % ((u32)((u16)(((u16)f2i_32(gv1[58U & 7])) != ((u64)a1))) | 1))))));
}
int main(void)
{
    gv0 = (u8)(((u8)59U) + vzero);
    mix_i((u64)f1((u8)(((u8)104U) + vzero), (u64)(((u64)31ULL) + vzero), (f64)(((f64)0.1) + vzero)));
    return (int)(ck % 251) == 97 ? 42 : 1;
}
