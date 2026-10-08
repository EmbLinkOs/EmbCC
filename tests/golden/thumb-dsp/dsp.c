/* The DSP extension's instructions, RUN: each one through inline asm on a
 * Cortex-M4 under QEMU, and the same program built on the host with -DHOST,
 * where every instruction is a C model written from the ARMv7-M ARM's
 * pseudocode. tests/golden/thumb-dsp.sh compares the two outputs line by
 * line, so an instruction that assembles to the wrong operation, reads its
 * operands from the wrong fields, or gets its GE flags wrong prints a
 * different hash than its model.
 *
 * Each line is one instruction: its name and a hash of its result over
 * every pair (and triple) of the inputs below, which are the lane edges --
 * 0x7fff and 0x8000 in each half, 0x7f/0x80/0xff in each byte, and
 * mixtures. The parallel adds and subtracts also hash the GE bits they set,
 * read back with `sel` in the same template. */
#ifdef HOST
#include <stdio.h>
static void writec(int c) { putchar(c); }
#else
void writec(int c);
#endif

typedef unsigned int u32;
typedef int s32;
typedef long long s64;
typedef unsigned long long u64;

static const u32 vals[] = {
    0x00000000u, 0x7fff8000u, 0x80017fffu, 0xffffffffu,
    0x12345678u, 0x80000000u, 0x00ff7f80u, 0xa5a55a5au,
    0x00010001u, 0x7f80ff01u
};
#define NV (int)(sizeof vals / sizeof vals[0])

static void puthex(u32 v)
{
    for (int k = 28; k >= 0; k -= 4)
        writec("0123456789abcdef"[(v >> k) & 15]);
}

static void line(const char *name, u32 h)
{
    while (*name)
        writec(*name++);
    writec(' ');
    puthex(h);
    writec('\n');
}

static u32 mix(u32 h, u32 v) { return (h ^ v) * 0x01000193u + (v >> 7); }

/* what `sel` chose after the last parallel op, by its GE bits */
static u32 g_sel;

#ifdef HOST
/* ---- the models ---------------------------------------------------- */
static s32 sh_(u32 x, int k) { s32 v = (s32)((x >> (16 * k)) & 0xffff); return v >= 0x8000 ? v - 0x10000 : v; }
static s32 uh_(u32 x, int k) { return (s32)((x >> (16 * k)) & 0xffff); }
static s32 sb_(u32 x, int k) { s32 v = (s32)((x >> (8 * k)) & 0xff); return v >= 0x80 ? v - 0x100 : v; }
static s32 ub_(u32 x, int k) { return (s32)((x >> (8 * k)) & 0xff); }
static s64 ssat_(s64 v, int n) { s64 hi = ((s64)1 << (n - 1)) - 1, lo = -((s64)1 << (n - 1)); return v > hi ? hi : v < lo ? lo : v; }
static s64 usat_(s64 v, int n) { s64 hi = ((s64)1 << n) - 1; return v > hi ? hi : v < 0 ? 0 : v; }
static u32 p16(s64 lo, s64 hi) { return ((u32)lo & 0xffffu) | ((u32)hi & 0xffffu) << 16; }
static u32 p8(s64 a, s64 b, s64 c, s64 d) { return ((u32)a & 0xff) | ((u32)b & 0xff) << 8 | ((u32)c & 0xff) << 16 | ((u32)d & 0xff) << 24; }
static u32 ror_(u32 x, int r) { return r ? (x >> r) | (x << (32 - r)) : x; }

/* the GE bits of a parallel op as `sel` reads them back: a byte of ones
 * where the bit is set */
static u32 g_ge;
static void ge16(int lo, int hi) { g_ge = (lo ? 0xffffu : 0) | (hi ? 0xffff0000u : 0); }
static void ge8(int a, int b, int c, int d) { g_ge = (a ? 0xffu : 0) | (b ? 0xff00u : 0) | (c ? 0xff0000u : 0) | (d ? 0xff000000u : 0); }

/* kind: 0 s, 1 q, 2 sh, 3 u, 4 uq, 5 uh; op: 0 add16 1 asx 2 sax 3 sub16 4 add8 5 sub8 */
static u32 par(int kind, int op, u32 a, u32 b)
{
    int sgn = kind < 3;
    s64 r[4];
    int n = op >= 4 ? 4 : 2;
    for (int k = 0; k < n; k++) {
        s64 x, y, v;
        if (n == 4) { x = sgn ? sb_(a, k) : ub_(a, k); y = sgn ? sb_(b, k) : ub_(b, k); }
        else {
            x = sgn ? sh_(a, k) : uh_(a, k);
            /* asx: lo = a.lo - b.hi, hi = a.hi + b.lo; sax the other way */
            int bk = (op == 1 || op == 2) ? 1 - k : k;
            y = sgn ? sh_(b, bk) : uh_(b, bk);
        }
        int sub = op == 3 || op == 5 || (op == 1 && k == 0) || (op == 2 && k == 1);
        v = sub ? x - y : x + y;
        r[k] = v;
    }
    int bits = n == 4 ? 8 : 16;
    for (int k = 0; k < n; k++) {
        int sub = op == 3 || op == 5 || (op == 1 && k == 0) || (op == 2 && k == 1);
        int g = kind == 0 ? r[k] >= 0 : sub ? r[k] >= 0 : r[k] >= ((s64)1 << bits);
        if (n == 4) g_ge = (k == 0 ? 0 : g_ge) | (g ? 0xffu << (8 * k) : 0);
        else if (k == 0) ge16(g, 0); else g_ge |= g ? 0xffff0000u : 0;
        switch (kind) {
        case 1: r[k] = ssat_(r[k], bits); break;
        case 4: r[k] = usat_(r[k], bits); break;
        case 2: case 5: r[k] >>= 1; break;
        default: break;
        }
    }
    (void)ge8;
    return n == 4 ? p8(r[0], r[1], r[2], r[3]) : p16(r[0], r[1]);
}

static s32 q32(s64 v) { return (s32)ssat_(v, 32); }
static u32 qadd(u32 m, u32 n) { return (u32)q32((s64)(s32)m + (s32)n); }
static u32 qsub(u32 m, u32 n) { return (u32)q32((s64)(s32)m - (s32)n); }
static u32 qdadd(u32 m, u32 n) { return (u32)q32((s64)(s32)m + q32(2 * (s64)(s32)n)); }
static u32 qdsub(u32 m, u32 n) { return (u32)q32((s64)(s32)m - q32(2 * (s64)(s32)n)); }

static u32 smlad(u32 n, u32 m, u32 a, int sub, int x)
{
    s64 p1 = (s64)sh_(n, 0) * sh_(m, x ? 1 : 0), p2 = (s64)sh_(n, 1) * sh_(m, x ? 0 : 1);
    return (u32)((s64)(s32)a + (sub ? p1 - p2 : p1 + p2));
}
static u64 smlald(u32 n, u32 m, u64 acc, int sub, int x)
{
    s64 p1 = (s64)sh_(n, 0) * sh_(m, x ? 1 : 0), p2 = (s64)sh_(n, 1) * sh_(m, x ? 0 : 1);
    return acc + (u64)(sub ? p1 - p2 : p1 + p2);
}
static u32 smlaxy(u32 n, u32 m, u32 a, int nt, int mt) { return (u32)((s64)sh_(n, nt) * sh_(m, mt) + (s32)a); }
static u32 smlaw(u32 n, u32 m, u32 a, int mt) { return (u32)(((s64)(s32)n * sh_(m, mt)) >> 16) + a; }
static u64 smlalxy(u32 n, u32 m, u64 acc, int nt, int mt) { return acc + (u64)((s64)sh_(n, nt) * sh_(m, mt)); }
static u32 smmla(u32 n, u32 m, u32 a, int sub, int rnd)
{
    u64 acc = (u64)a << 32, p = (u64)((s64)(s32)n * (s32)m);
    u64 v = sub ? acc - p : acc + p;
    if (rnd) v += 0x80000000u;
    return (u32)(v >> 32);
}
static u32 usada8(u32 n, u32 m, u32 a)
{
    u32 s = a;
    for (int k = 0; k < 4; k++) { s32 d = ub_(n, k) - ub_(m, k); s += (u32)(d < 0 ? -d : d); }
    return s;
}
static u32 sxtb16(u32 m, int r) { u32 x = ror_(m, r); return p16(sb_(x, 0), sb_(x, 2)); }
static u32 uxtab16(u32 n, u32 m, int r) { u32 x = ror_(m, r); return p16(uh_(n, 0) + ub_(x, 0), uh_(n, 1) + ub_(x, 2)); }
static u32 sxtah(u32 n, u32 m, int r) { return n + (u32)sh_(ror_(m, r), 0); }
static u32 uxtab(u32 n, u32 m, int r) { return n + (u32)ub_(ror_(m, r), 0); }
static u32 pkhbt(u32 n, u32 m, int s) { return (n & 0xffffu) | ((m << s) & 0xffff0000u); }
static u32 pkhtb(u32 n, u32 m, int s) { s64 v = (s32)m; v >>= s; return (n & 0xffff0000u) | ((u32)v & 0xffffu); }

#define PAR(name, kind, op) static u32 name(u32 a, u32 b) { u32 r = par(kind, op, a, b); \
    g_sel = (a & g_ge) | (b & ~g_ge); return r; }
#define SSAT(n, v, txt, sv) ((u32)ssat_(sv, n))
#define USAT(n, v, txt, sv) ((u32)usat_(sv, n))
#define SSAT16(n, v) p16(ssat_(sh_(v, 0), n), ssat_(sh_(v, 1), n))
#define USAT16(n, v) p16(usat_(sh_(v, 0), n), usat_(sh_(v, 1), n))
#else
/* ---- the instructions -------------------------------------------- */
/* the op, then `sel` between its own operands by the GE bits it set: in
 * one template, so nothing between the two can change the flags */
#define PAR(name, kind, op) static u32 name(u32 a, u32 b) { u32 r, s; \
    __asm__(#name " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); \
    __asm__ volatile(#name " %0, %1, %2\n\tsel %0, %1, %2" : "=&r"(s) : "r"(a), "r"(b)); \
    g_sel = s; return r; }
#define A3(name) static u32 name##_(u32 a, u32 b) { u32 r; \
    __asm__(#name " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }
#define A4(name) static u32 name##_(u32 a, u32 b, u32 c) { u32 r; \
    __asm__(#name " %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(c)); return r; }
#define L4(name) static u64 name##_(u32 a, u32 b, u64 acc) { u32 lo = (u32)acc, hi = (u32)(acc >> 32); \
    __asm__(#name " %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); return (u64)hi << 32 | lo; }
A3(qadd) A3(qsub) A3(qdadd) A3(qdsub)
A4(smlad) A4(smladx) A4(smlsd) A4(smlsdx) A3(smuad) A3(smuadx) A3(smusd) A3(smusdx)
L4(smlald) L4(smlaldx) L4(smlsld) L4(smlsldx)
A4(smlabb) A4(smlatb) A4(smlabt) A4(smlatt) A3(smulbb) A3(smultb) A3(smulbt) A3(smultt)
A4(smlawb) A4(smlawt) A3(smulwb) A3(smulwt)
L4(smlalbb) L4(smlaltb) L4(smlalbt) L4(smlaltt)
A4(smmla) A4(smmlar) A4(smmls) A4(smmlsr) A3(smmul) A3(smmulr)
A4(usada8) A3(usad8)
static u32 qadd(u32 m, u32 n) { return qadd_(m, n); }
static u32 qsub(u32 m, u32 n) { return qsub_(m, n); }
static u32 qdadd(u32 m, u32 n) { return qdadd_(m, n); }
static u32 qdsub(u32 m, u32 n) { return qdsub_(m, n); }
static u32 smlad(u32 n, u32 m, u32 a, int sub, int x)
{
    return sub ? (x ? smlsdx_(n, m, a) : smlsd_(n, m, a)) : (x ? smladx_(n, m, a) : smlad_(n, m, a));
}
static u64 smlald(u32 n, u32 m, u64 acc, int sub, int x)
{
    return sub ? (x ? smlsldx_(n, m, acc) : smlsld_(n, m, acc)) : (x ? smlaldx_(n, m, acc) : smlald_(n, m, acc));
}
static u32 smlaxy(u32 n, u32 m, u32 a, int nt, int mt)
{
    return nt ? (mt ? smlatt_(n, m, a) : smlatb_(n, m, a)) : (mt ? smlabt_(n, m, a) : smlabb_(n, m, a));
}
static u32 smlaw(u32 n, u32 m, u32 a, int mt) { return mt ? smlawt_(n, m, a) : smlawb_(n, m, a); }
static u64 smlalxy(u32 n, u32 m, u64 acc, int nt, int mt)
{
    return nt ? (mt ? smlaltt_(n, m, acc) : smlaltb_(n, m, acc)) : (mt ? smlalbt_(n, m, acc) : smlalbb_(n, m, acc));
}
static u32 smmla(u32 n, u32 m, u32 a, int sub, int rnd)
{
    return sub ? (rnd ? smmlsr_(n, m, a) : smmls_(n, m, a)) : (rnd ? smmlar_(n, m, a) : smmla_(n, m, a));
}
static u32 usada8(u32 n, u32 m, u32 a) { return usada8_(n, m, a); }
static u32 sxtb16(u32 m, int r)
{
    u32 x;
    switch (r) {
    case 0: __asm__("sxtb16 %0, %1" : "=r"(x) : "r"(m)); break;
    case 8: __asm__("sxtb16 %0, %1, ror #8" : "=r"(x) : "r"(m)); break;
    /* CMSIS's spelling: an "i" operand, substituted without the # */
    case 16: __asm__("sxtb16 %0, %1, ROR %2" : "=r"(x) : "r"(m), "i"(16)); break;
    default: __asm__("sxtb16 %0, %1, ror #24" : "=r"(x) : "r"(m)); break;
    }
    return x;
}
static u32 uxtab16(u32 n, u32 m, int r)
{
    u32 x;
    switch (r) {
    case 0: __asm__("uxtab16 %0, %1, %2" : "=r"(x) : "r"(n), "r"(m)); break;
    case 8: __asm__("uxtab16 %0, %1, %2, ror #8" : "=r"(x) : "r"(n), "r"(m)); break;
    case 16: __asm__("uxtab16 %0, %1, %2, ror #16" : "=r"(x) : "r"(n), "r"(m)); break;
    default: __asm__("uxtab16 %0, %1, %2, ror #24" : "=r"(x) : "r"(n), "r"(m)); break;
    }
    return x;
}
static u32 sxtah(u32 n, u32 m, int r)
{
    u32 x;
    if (r) __asm__("sxtah %0, %1, %2, ror #16" : "=r"(x) : "r"(n), "r"(m));
    else   __asm__("sxtah %0, %1, %2" : "=r"(x) : "r"(n), "r"(m));
    return x;
}
static u32 uxtab(u32 n, u32 m, int r)
{
    u32 x;
    if (r) __asm__("uxtab %0, %1, %2, ror #24" : "=r"(x) : "r"(n), "r"(m));
    else   __asm__("uxtab %0, %1, %2" : "=r"(x) : "r"(n), "r"(m));
    return x;
}
static u32 pkhbt(u32 n, u32 m, int s)
{
    u32 x;
    if (s) __asm__("pkhbt %0, %1, %2, lsl %3" : "=r"(x) : "r"(n), "r"(m), "I"(11));
    else   __asm__("pkhbt %0, %1, %2" : "=r"(x) : "r"(n), "r"(m));
    return x;
}
static u32 pkhtb(u32 n, u32 m, int s)
{
    u32 x;
    if (s == 32) __asm__("pkhtb %0, %1, %2, asr #32" : "=r"(x) : "r"(n), "r"(m));
    else if (s) __asm__("pkhtb %0, %1, %2, asr #5" : "=r"(x) : "r"(n), "r"(m));
    else __asm__("pkhtb %0, %1, %2" : "=r"(x) : "r"(n), "r"(m));
    return x;
}
#define SSAT(n, v, txt, sv) ({ u32 r_; __asm__("ssat %0, %1, %2" txt : "=r"(r_) : "I"(n), "r"(v)); r_; })
#define USAT(n, v, txt, sv) ({ u32 r_; __asm__("usat %0, %1, %2" txt : "=r"(r_) : "I"(n), "r"(v)); r_; })
#define SSAT16(n, v) ({ u32 r_; __asm__("ssat16 %0, %1, %2" : "=r"(r_) : "I"(n), "r"(v)); r_; })
#define USAT16(n, v) ({ u32 r_; __asm__("usat16 %0, %1, %2" : "=r"(r_) : "I"(n), "r"(v)); r_; })
#endif

PAR(sadd16, 0, 0) PAR(sasx, 0, 1) PAR(ssax, 0, 2) PAR(ssub16, 0, 3) PAR(sadd8, 0, 4) PAR(ssub8, 0, 5)
PAR(qadd16, 1, 0) PAR(qasx, 1, 1) PAR(qsax, 1, 2) PAR(qsub16, 1, 3) PAR(qadd8, 1, 4) PAR(qsub8, 1, 5)
PAR(shadd16, 2, 0) PAR(shasx, 2, 1) PAR(shsax, 2, 2) PAR(shsub16, 2, 3) PAR(shadd8, 2, 4) PAR(shsub8, 2, 5)
PAR(uadd16, 3, 0) PAR(uasx, 3, 1) PAR(usax, 3, 2) PAR(usub16, 3, 3) PAR(uadd8, 3, 4) PAR(usub8, 3, 5)
PAR(uqadd16, 4, 0) PAR(uqasx, 4, 1) PAR(uqsax, 4, 2) PAR(uqsub16, 4, 3) PAR(uqadd8, 4, 4) PAR(uqsub8, 4, 5)
PAR(uhadd16, 5, 0) PAR(uhasx, 5, 1) PAR(uhsax, 5, 2) PAR(uhsub16, 5, 3) PAR(uhadd8, 5, 4) PAR(uhsub8, 5, 5)

typedef u32 (*bin_fn)(u32, u32);
static const struct { const char *name; bin_fn f; int ge; } bins[] = {
    { "sadd16", sadd16, 1 }, { "sasx", sasx, 1 }, { "ssax", ssax, 1 },
    { "ssub16", ssub16, 1 }, { "sadd8", sadd8, 1 }, { "ssub8", ssub8, 1 },
    { "qadd16", qadd16, 0 }, { "qasx", qasx, 0 }, { "qsax", qsax, 0 },
    { "qsub16", qsub16, 0 }, { "qadd8", qadd8, 0 }, { "qsub8", qsub8, 0 },
    { "shadd16", shadd16, 0 }, { "shasx", shasx, 0 }, { "shsax", shsax, 0 },
    { "shsub16", shsub16, 0 }, { "shadd8", shadd8, 0 }, { "shsub8", shsub8, 0 },
    { "uadd16", uadd16, 1 }, { "uasx", uasx, 1 }, { "usax", usax, 1 },
    { "usub16", usub16, 1 }, { "uadd8", uadd8, 1 }, { "usub8", usub8, 1 },
    { "uqadd16", uqadd16, 0 }, { "uqasx", uqasx, 0 }, { "uqsax", uqsax, 0 },
    { "uqsub16", uqsub16, 0 }, { "uqadd8", uqadd8, 0 }, { "uqsub8", uqsub8, 0 },
    { "uhadd16", uhadd16, 0 }, { "uhasx", uhasx, 0 }, { "uhsax", uhsax, 0 },
    { "uhsub16", uhsub16, 0 }, { "uhadd8", uhadd8, 0 }, { "uhsub8", uhsub8, 0 },
    { "qadd", qadd, 0 }, { "qsub", qsub, 0 }, { "qdadd", qdadd, 0 },
    { "qdsub", qdsub, 0 }
};

int main(void)
{
    for (unsigned k = 0; k < sizeof bins / sizeof bins[0]; k++) {
        u32 h = 0, g = 0;
        for (int i = 0; i < NV; i++)
            for (int j = 0; j < NV; j++) {
                h = mix(h, bins[k].f(vals[i], vals[j]));
                if (bins[k].ge)
                    g = mix(g, g_sel);
            }
        line(bins[k].name, h);
        if (bins[k].ge)
            line("  sel", g);
    }
    {
        /* the multiplies: every pair, each with a third value as Ra or the
         * accumulator */
        static const char *const mnames[] = {
            "smlad", "smladx", "smlsd", "smlsdx", "smlabb", "smlabt",
            "smlatb", "smlatt", "smlawb", "smlawt", "smmla", "smmlar",
            "smmls", "smmlsr", "usada8", "smlald", "smlaldx", "smlsld",
            "smlsldx", "smlalbb", "smlalbt", "smlaltb", "smlaltt"
        };
        for (int f = 0; f < 23; f++) {
            u32 h = 0;
            for (int i = 0; i < NV; i++)
                for (int j = 0; j < NV; j++) {
                    u32 n = vals[i], m = vals[j], a = vals[(i + j + 3) % NV];
                    u64 acc = (u64)vals[(i + 2 * j + 1) % NV] << 32 | a;
                    u32 r;
                    u64 r64;
                    switch (f) {
                    case 0: case 1: case 2: case 3:
                        r = smlad(n, m, a, f >> 1, f & 1); break;
                    case 4: case 5: case 6: case 7:
                        r = smlaxy(n, m, a, (f - 4) >> 1, f & 1); break;
                    case 8: case 9: r = smlaw(n, m, a, f & 1); break;
                    case 10: case 11: case 12: case 13:
                        r = smmla(n, m, a, (f - 10) >> 1, f & 1); break;
                    case 14: r = usada8(n, m, a); break;
                    case 15: case 16: case 17: case 18:
                        r64 = smlald(n, m, acc, (f - 15) >> 1, f & 1);
                        r = (u32)r64 ^ (u32)(r64 >> 32) * 3u; break;
                    default:
                        r64 = smlalxy(n, m, acc, (f - 19) >> 1, f & 1);
                        r = (u32)r64 ^ (u32)(r64 >> 32) * 3u; break;
                    }
                    h = mix(h, r);
                }
            line(mnames[f], h);
        }
    }
    {
        u32 h[12] = { 0 };
        for (int i = 0; i < NV; i++) {
            u32 v = vals[i];
            for (int j = 0; j < NV; j++) {
                u32 m = vals[j];
                h[0] = mix(h[0], sxtb16(m, (j & 3) * 8));
                h[1] = mix(h[1], uxtab16(v, m, (j & 3) * 8));
                h[2] = mix(h[2], sxtah(v, m, (j & 1) * 16));
                h[3] = mix(h[3], uxtab(v, m, (j & 1) * 24));
                h[4] = mix(h[4], pkhbt(v, m, (j & 1) * 11));
                h[5] = mix(h[5], pkhtb(v, m, (j % 3 == 0) ? 0 : (j % 3 == 1) ? 5 : 32));
            }
            h[6] = mix(h[6], SSAT(8, v, "", (s64)(s32)v));
            h[6] = mix(h[6], SSAT(1, v, ", lsl #3", (s64)(s32)(v << 3)));
            h[6] = mix(h[6], SSAT(32, v, ", asr #7", (s64)((s32)v >> 7)));
            h[6] = mix(h[6], SSAT(13, v, ", lsl #17", (s64)(s32)(v << 17)));
            h[7] = mix(h[7], USAT(8, v, "", (s64)(s32)v));
            h[7] = mix(h[7], USAT(0, v, ", lsl #1", (s64)(s32)(v << 1)));
            h[7] = mix(h[7], USAT(31, v, ", asr #31", (s64)((s32)v >> 31)));
            h[7] = mix(h[7], USAT(17, v, ", asr #3", (s64)((s32)v >> 3)));
            h[8] = mix(h[8], SSAT16(1, v));
            h[8] = mix(h[8], SSAT16(9, v));
            h[8] = mix(h[8], SSAT16(16, v));
            h[9] = mix(h[9], USAT16(0, v));
            h[9] = mix(h[9], USAT16(7, v));
            h[9] = mix(h[9], USAT16(15, v));
        }
        line("sxtb16", h[0]); line("uxtab16", h[1]); line("sxtah", h[2]);
        line("uxtab", h[3]); line("pkhbt", h[4]); line("pkhtb", h[5]);
        line("ssat", h[6]); line("usat", h[7]); line("ssat16", h[8]);
        line("usat16", h[9]);
    }
    {
        const char *s = "==END==\n";
        while (*s)
            writec(*s++);
    }
    return 0;
}
