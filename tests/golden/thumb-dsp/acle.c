/* <arm_acle.h>'s DSP and SIMD32 intrinsics, every one, over pairs of lane-
 * edge values: built by EmbCC (its header: inline asm) and by clang (its
 * header: builtins), both linked into the same harness and run on a
 * Cortex-M4, and the outputs compared by tests/golden/thumb-dsp.sh. */
#include <arm_acle.h>
#include <stdint.h>

void writec(int c);

static const uint32_t vals[] = {
    0x00000000u, 0x7fff8000u, 0x80017fffu, 0xffffffffu, 0x12345678u,
    0x80000000u, 0x00ff7f80u, 0xa5a55a5au, 0x00010001u, 0x7f80ff01u
};
#define NV (int)(sizeof vals / sizeof vals[0])

static uint32_t h;
static void mix(uint32_t v) { h = (h ^ v) * 0x01000193u + (v >> 7); }
static void line(const char *name)
{
    while (*name) writec(*name++);
    writec(' ');
    for (int k = 28; k >= 0; k -= 4) writec("0123456789abcdef"[(h >> k) & 15]);
    writec('\n');
    h = 0;
}

#define T2(f) do { for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) \
    mix((uint32_t)f(vals[i], vals[j])); line(#f); } while (0)
/* a GE-setting op, then __sel by the bits it set */
#define TG(f) do { for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) { \
    mix((uint32_t)f(vals[i], vals[j])); mix(__sel(vals[j], vals[i])); } line(#f); } while (0)
#define T3(f) do { for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) \
    mix((uint32_t)f(vals[i], vals[j], vals[(i + j + 3) % NV])); line(#f); } while (0)
#define TL(f) do { for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) { \
    uint64_t r = (uint64_t)f(vals[i], vals[j], (int64_t)((uint64_t)vals[(i + 2 * j + 1) % NV] << 32 | vals[(i + j) % NV])); \
    mix((uint32_t)r); mix((uint32_t)(r >> 32)); } line(#f); } while (0)

int main(void)
{
    for (int i = 0; i < NV; i++) {
        int32_t v = (int32_t)vals[i];
        mix((uint32_t)__ssat(v, 1)); mix((uint32_t)__ssat(v, 8));
        mix((uint32_t)__ssat(v, 32)); mix(__usat(v, 0)); mix(__usat(v, 7));
        mix(__usat(v, 31)); mix((uint32_t)__ssat16(v, 1));
        mix((uint32_t)__ssat16(v, 12)); mix(__usat16(v, 0)); mix(__usat16(v, 15));
        mix((uint32_t)__sxtb16(v)); mix((uint32_t)__uxtb16(v)); mix((uint32_t)__qdbl(v));
    }
    line("sat");
    T2(__qadd); T2(__qsub);
    T3(__smlabb); T3(__smlabt); T3(__smlatb); T3(__smlatt); T3(__smlawb); T3(__smlawt);
    T2(__sxtab16); T2(__uxtab16);
    T2(__qadd8); T2(__qsub8); TG(__sadd8); T2(__shadd8); T2(__shsub8); TG(__ssub8);
    TG(__uadd8); T2(__uhadd8); T2(__uhsub8); T2(__uqadd8); T2(__uqsub8); TG(__usub8);
    T2(__usad8); T3(__usada8);
    T2(__qadd16); T2(__qasx); T2(__qsax); T2(__qsub16); TG(__sadd16); TG(__sasx);
    T2(__shadd16); T2(__shasx); T2(__shsax); T2(__shsub16); TG(__ssax); TG(__ssub16);
    TG(__uadd16); TG(__uasx); T2(__uhadd16); T2(__uhasx); T2(__uhsax); T2(__uhsub16);
    T2(__uqadd16); T2(__uqasx); T2(__uqsax); T2(__uqsub16); TG(__usax); TG(__usub16);
    T3(__smlad); T3(__smladx); TL(__smlald); TL(__smlaldx); T3(__smlsd); T3(__smlsdx);
    TL(__smlsld); TL(__smlsldx); T2(__smuad); T2(__smuadx); T2(__smusd); T2(__smusdx);
    {
        const char *s = "==END==\n";
        while (*s) writec(*s++);
    }
    return 0;
}
