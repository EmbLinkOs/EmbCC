/* CMSIS-Core's SIMD intrinsics (cmsis_gcc.h, selected by __ARM_FEATURE_DSP
 * and __ARM_ARCH_7EM__), every one, built by EmbCC and by clang from ARM's
 * header as it ships, linked into the same harness, run on a Cortex-M4, and
 * the outputs compared by tests/golden/thumb-dsp.sh. Before the DSP macros
 * and instructions this header's whole SIMD section was compiled out on
 * EmbCC, and a CMSIS-DSP build took its plain-C paths or failed. */
#include <stdint.h>
#include "cmsis_compiler.h"

#if !defined(__ARM_FEATURE_DSP) || !defined(__ARM_ARCH_7EM__)
#error "a Cortex-M4 build without the DSP macros"
#endif

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
#define TG(f) do { for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) { \
    mix((uint32_t)f(vals[i], vals[j])); mix(__SEL(vals[j], vals[i])); } line(#f); } while (0)
#define T3(f) do { for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) \
    mix((uint32_t)f(vals[i], vals[j], vals[(i + j + 3) % NV])); line(#f); } while (0)
#define TL(f) do { for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) { \
    uint64_t r = f(vals[i], vals[j], (uint64_t)vals[(i + 2 * j + 1) % NV] << 32 | vals[(i + j) % NV]); \
    mix((uint32_t)r); mix((uint32_t)(r >> 32)); } line(#f); } while (0)

int main(void)
{
    TG(__SADD8); T2(__QADD8); T2(__SHADD8); TG(__UADD8); T2(__UQADD8); T2(__UHADD8);
    TG(__SSUB8); T2(__QSUB8); T2(__SHSUB8); TG(__USUB8); T2(__UQSUB8); T2(__UHSUB8);
    TG(__SADD16); T2(__QADD16); T2(__SHADD16); TG(__UADD16); T2(__UQADD16); T2(__UHADD16);
    TG(__SSUB16); T2(__QSUB16); T2(__SHSUB16); TG(__USUB16); T2(__UQSUB16); T2(__UHSUB16);
    TG(__SASX); T2(__QASX); T2(__SHASX); TG(__UASX); T2(__UQASX); T2(__UHASX);
    TG(__SSAX); T2(__QSAX); T2(__SHSAX); TG(__USAX); T2(__UQSAX); T2(__UHSAX);
    T2(__USAD8); T3(__USADA8); T2(__UXTAB16); T2(__SXTAB16);
    T2(__SMUAD); T2(__SMUADX); T3(__SMLAD); T3(__SMLADX);
    TL(__SMLALD); TL(__SMLALDX); T2(__SMUSD); T2(__SMUSDX); T3(__SMLSD); T3(__SMLSDX);
    TL(__SMLSLD); TL(__SMLSLDX); T2(__QADD); T2(__QSUB); T3(__SMMLA);
    for (int i = 0; i < NV; i++) {
        uint32_t v = vals[i], w = vals[(i + 3) % NV];
        mix((uint32_t)__SSAT((int32_t)v, 1)); mix((uint32_t)__SSAT((int32_t)v, 16));
        mix(__USAT((int32_t)v, 0)); mix(__USAT((int32_t)v, 12));
        mix(__SSAT16(v, 1)); mix(__SSAT16(v, 9)); mix(__USAT16(v, 0)); mix(__USAT16(v, 15));
        mix(__UXTB16(v)); mix(__SXTB16(v));
        mix(__SXTB16_RORn(v, 8)); mix(__SXTB16_RORn(v, 16)); mix(__SXTB16_RORn(v, 24));
        mix(__SXTAB16_RORn(w, v, 8)); mix(__SXTAB16_RORn(w, v, 24));
        mix(__PKHBT(v, w, 0)); mix(__PKHBT(v, w, 13)); mix(__PKHTB(v, w, 0));
        mix(__PKHTB(v, w, 7)); mix(__PKHTB(v, w, 31));
    }
    line("sat/pack/extend");
    {
        const char *s = "==END==\n";
        while (*s) writec(*s++);
    }
    return 0;
}
