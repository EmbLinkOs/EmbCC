/* The multiplies, reversals, rrx, bit fields, ldrd/strd and exclusives
 * the assembler takes beyond the DSP extension, RUN: each through inline
 * asm on a Cortex-M4 (Thumb) and on a Cortex-A15 (ARM state) under QEMU,
 * and the same program built on the host with -DHOST, where every
 * instruction is a C model of its pseudocode. tests/golden/arm-asm-more.sh
 * compares the outputs line by line, so an instruction that assembles to
 * the wrong operation, or reads an operand from the wrong field, prints a
 * different hash than its model.
 *
 * Each line is one instruction: its name and a hash of its result over
 * every pair (and triple) of the inputs below. */
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
    0x00010001u, 0x7f80ff01u, 0x00000001u, 0xfedcba98u
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
static u32 mix64(u32 h, u64 v) { return mix(mix(h, (u32)v), (u32)(v >> 32)); }

/* the fields every bit-field instruction is run with: lsb, width */
#define FIELDS(X) X(0, 1) X(0, 32) X(31, 1) X(4, 8) X(13, 19) X(7, 3) X(16, 16)

#ifdef HOST
/* ---- the models ---------------------------------------------------- */
static u32 mla(u32 n, u32 m, u32 a) { return a + n * m; }
static u32 mls(u32 n, u32 m, u32 a) { return a - n * m; }
static u64 smull(u32 n, u32 m) { return (u64)((s64)(s32)n * (s32)m); }
static u64 umull(u32 n, u32 m) { return (u64)n * m; }
static u64 smlal(u32 n, u32 m, u64 acc) { return acc + smull(n, m); }
static u64 umlal(u32 n, u32 m, u64 acc) { return acc + umull(n, m); }
static u64 umaal(u32 n, u32 m, u64 acc) { return (u64)n * m + (u32)acc + (u32)(acc >> 32); }
static u32 rev16(u32 m) { return ((m >> 8) & 0x00ff00ffu) | ((m << 8) & 0xff00ff00u); }
static u32 revsh(u32 m) { u32 v = ((m >> 8) & 0xffu) | ((m & 0xffu) << 8); return v & 0x8000u ? v | 0xffff0000u : v; }
/* rrx with the carry from cmp a, b (a >= b unsigned) */
static u32 rrx(u32 m, u32 a, u32 b) { return (m >> 1) | (a >= b ? 0x80000000u : 0); }
/* rrxs: the result, and the flags after it -- N and Z from the result, C
 * the bit shifted out (bit 0 of m), V the cmp's */
static u32 rrxs(u32 m, u32 a, u32 b, u32 *c)
{
    u32 r = rrx(m, a, b), v = ((a ^ b) & (a ^ (a - b))) >> 31;
    *c = (r & 0x80000000u) | (r == 0) << 30 | (m & 1) << 29 | v << 28;
    return r;
}
static u32 mask(int w) { return w == 32 ? 0xffffffffu : (1u << w) - 1; }
static u32 bfi(u32 d, u32 n, int lsb, int w) { u32 k = mask(w) << lsb; return (d & ~k) | ((n << lsb) & k); }
static u32 bfc(u32 d, int lsb, int w) { return d & ~(mask(w) << lsb); }
static u32 ubfx(u32 n, int lsb, int w) { return (n >> lsb) & mask(w); }
static u32 sbfx(u32 n, int lsb, int w) { u32 v = ubfx(n, lsb, w); return w < 32 && (v >> (w - 1)) ? v | ~mask(w) : v; }
#define BF(lsb, w) static u32 bfi_##lsb##_##w(u32 d, u32 n) { return bfi(d, n, lsb, w); } \
    static u32 bfc_##lsb##_##w(u32 d) { return bfc(d, lsb, w); } \
    static u32 ubfx_##lsb##_##w(u32 n) { return ubfx(n, lsb, w); } \
    static u32 sbfx_##lsb##_##w(u32 n) { return sbfx(n, lsb, w); }
FIELDS(BF)

/* the pairs: what was loaded or the buffer after the store, and how far
 * the base moved */
static u32 buf[8];
static u32 ldrd_off(int k, u32 *a, u32 *b) { *a = buf[k + 2]; *b = buf[k + 3]; return 0; }
static u32 ldrd_pre(int k, u32 *a, u32 *b) { *a = buf[k + 2]; *b = buf[k + 3]; return 8; }
static u32 ldrd_post(int k, u32 *a, u32 *b) { *a = buf[k]; *b = buf[k + 1]; return 0xfffffff8u; }
static u32 strd_off(int k, u32 a, u32 b) { buf[k + 2] = a; buf[k + 3] = b; return 0; }
static u32 strd_pre(int k, u32 a, u32 b) { buf[k - 2] = a; buf[k - 1] = b; return 0xfffffff8u; }
static u32 strd_post(int k, u32 a, u32 b) { buf[k] = a; buf[k + 1] = b; return 16; }
/* the exclusives: a loop that adds b to the value, and its status */
static u32 exb(unsigned char *p, u32 b) { *p = (unsigned char)(*p + b); return 0; }
static u32 exh(unsigned short *p, u32 b) { *p = (unsigned short)(*p + b); return 0; }
static u64 exd(u64 *p, u32 b) { *p += b; return *p; }
#else
/* ---- the instructions -------------------------------------------- */
static u32 mla(u32 n, u32 m, u32 a) { u32 r; __asm__("mla %0, %1, %2, %3" : "=r"(r) : "r"(n), "r"(m), "r"(a)); return r; }
static u32 mls(u32 n, u32 m, u32 a) { u32 r; __asm__("mls %0, %1, %2, %3" : "=r"(r) : "r"(n), "r"(m), "r"(a)); return r; }
#define LM(name) static u64 name(u32 n, u32 m) { u32 lo, hi; \
    __asm__(#name " %0, %1, %2, %3" : "=&r"(lo), "=&r"(hi) : "r"(n), "r"(m)); return (u64)hi << 32 | lo; }
#define LA(name) static u64 name(u32 n, u32 m, u64 acc) { u32 lo = (u32)acc, hi = (u32)(acc >> 32); \
    __asm__(#name " %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(n), "r"(m)); return (u64)hi << 32 | lo; }
LM(smull) LM(umull) LA(smlal) LA(umlal) LA(umaal)
static u32 rev16(u32 m) { u32 r; __asm__("rev16 %0, %1" : "=r"(r) : "r"(m)); return r; }
static u32 revsh(u32 m) { u32 r; __asm__("revsh %0, %1" : "=r"(r) : "r"(m)); return r; }
static u32 rrx(u32 m, u32 a, u32 b)
{
    u32 r;
    __asm__("cmp %2, %3\n\trrx %0, %1" : "=r"(r) : "r"(m), "r"(a), "r"(b) : "cc");
    return r;
}
/* the flags rrxs sets, read back with mrs (apsr names them in both
 * states) */
static u32 rrxs(u32 m, u32 a, u32 b, u32 *c)
{
    u32 r, fl;
    __asm__("cmp %3, %4\n\trrxs %0, %2\n\tmrs %1, apsr"
            : "=&r"(r), "=&r"(fl) : "r"(m), "r"(a), "r"(b) : "cc");
    *c = fl & 0xf0000000u;
    return r;
}
#define BF(lsb, w) \
    static u32 bfi_##lsb##_##w(u32 d, u32 n) { __asm__("bfi %0, %1, #" #lsb ", #" #w : "+r"(d) : "r"(n)); return d; } \
    static u32 bfc_##lsb##_##w(u32 d) { __asm__("bfc %0, #" #lsb ", #" #w : "+r"(d)); return d; } \
    static u32 ubfx_##lsb##_##w(u32 n) { u32 r; __asm__("ubfx %0, %1, #" #lsb ", #" #w : "=r"(r) : "r"(n)); return r; } \
    static u32 sbfx_##lsb##_##w(u32 n) { u32 r; __asm__("sbfx %0, %1, %2, %3" : "=r"(r) : "r"(n), "i"(lsb), "i"(w)); return r; }
FIELDS(BF)

/* each form through a base pointer at buf + k, returning how far the
 * base moved (0 for offset addressing) */
static u32 buf[8];
static u32 ldrd_off(int k, u32 *a, u32 *b)
{
    u32 *p = buf + k, *q = p, x, y;
    __asm__ volatile("ldrd %0, %1, [%2, #8]" : "=&r"(x), "=&r"(y) : "r"(p) : "memory");
    *a = x; *b = y;
    return (u32)((char *)p - (char *)q);
}
static u32 ldrd_pre(int k, u32 *a, u32 *b)
{
    u32 *p = buf + k, *q = p, x, y;
    __asm__ volatile("ldrd %0, %1, [%2, #8]!" : "=&r"(x), "=&r"(y), "+r"(p) : : "memory");
    *a = x; *b = y;
    return (u32)((char *)p - (char *)q);
}
static u32 ldrd_post(int k, u32 *a, u32 *b)
{
    u32 *p = buf + k, *q = p, x, y;
    __asm__ volatile("ldrd %0, %1, [%2], #-8" : "=&r"(x), "=&r"(y), "+r"(p) : : "memory");
    *a = x; *b = y;
    return (u32)((char *)p - (char *)q);
}
/* (ARM state's pair is an even register and the next: r2, r3) */
static u32 strd_off(int k, u32 a, u32 b)
{
    register u32 x __asm__("r2") = a;
    register u32 y __asm__("r3") = b;
    u32 *p = buf + k;
    __asm__ volatile("strd %1, %2, [%0, #8]" : : "r"(p), "r"(x), "r"(y) : "memory");
    return 0;
}
static u32 strd_pre(int k, u32 a, u32 b)
{
    register u32 x __asm__("r2") = a;
    register u32 y __asm__("r3") = b;
    u32 *p = buf + k, *q = p;
    __asm__ volatile("strd %1, %2, [%0, #-8]!" : "+r"(p) : "r"(x), "r"(y) : "memory");
    return (u32)((char *)p - (char *)q);
}
static u32 strd_post(int k, u32 a, u32 b)
{
    register u32 x __asm__("r2") = a;
    register u32 y __asm__("r3") = b;
    u32 *p = buf + k, *q = p;
    __asm__ volatile("strd %1, %2, [%0], #16" : "+r"(p) : "r"(x), "r"(y) : "memory");
    return (u32)((char *)p - (char *)q);
}
/* the exclusives: add b to the value, a load/store-exclusive pair retried
 * from C until the store succeeds (inline asm here takes no labels) */
static u32 exb(unsigned char *p, u32 b)
{
    u32 v, st;
    do __asm__ volatile("ldrexb %0, [%2]\n\tadd %0, %0, %3\n\tstrexb %1, %0, [%2]"
                        : "=&r"(v), "=&r"(st) : "r"(p), "r"(b) : "memory");
    while (st);
    __asm__ volatile("clrex");
    return st;
}
static u32 exh(unsigned short *p, u32 b)
{
    u32 v, st;
    do __asm__ volatile("ldrexh %0, [%2]\n\tadd %0, %0, %3\n\tstrexh %1, %0, [%2]"
                        : "=&r"(v), "=&r"(st) : "r"(p), "r"(b) : "memory");
    while (st);
    return st;
}
#ifndef __thumb__
/* ARM state's doubleword exclusive, through the pair r2:r3: the value
 * it stored */
static u64 exd(u64 *p, u32 b)
{
    register u32 lo __asm__("r2");
    register u32 hi __asm__("r3");
    u32 st;
    do __asm__ volatile("ldrexd %0, %1, [%3]\n\tadds %0, %0, %4\n\tadc %1, %1, #0\n\t"
                        "strexd %2, %0, %1, [%3]"
                        : "=&r"(lo), "=&r"(hi), "=&r"(st) : "r"(p), "r"(b) : "cc", "memory");
    while (st);
    return (u64)hi << 32 | lo;
}
#endif
#endif

int main(void)
{
    {
        u32 h[7] = { 0 };
        for (int i = 0; i < NV; i++)
            for (int j = 0; j < NV; j++) {
                u32 n = vals[i], m = vals[j], a = vals[(i + j + 3) % NV];
                u64 acc = (u64)vals[(i + 2 * j + 1) % NV] << 32 | a;
                h[0] = mix(h[0], mla(n, m, a));
                h[1] = mix(h[1], mls(n, m, a));
                h[2] = mix64(h[2], smull(n, m));
                h[3] = mix64(h[3], umull(n, m));
                h[4] = mix64(h[4], smlal(n, m, acc));
                h[5] = mix64(h[5], umlal(n, m, acc));
                h[6] = mix64(h[6], umaal(n, m, acc));
            }
        line("mla", h[0]); line("mls", h[1]); line("smull", h[2]);
        line("umull", h[3]); line("smlal", h[4]); line("umlal", h[5]);
        line("umaal", h[6]);
    }
    {
        u32 h[4] = { 0 };
        for (int i = 0; i < NV; i++) {
            h[0] = mix(h[0], rev16(vals[i]));
            h[1] = mix(h[1], revsh(vals[i]));
            for (int j = 0; j < NV; j++) {
                u32 c = 0, a = vals[j], b = vals[(i + j) % NV];
                h[2] = mix(h[2], rrx(vals[i], a, b));
                h[3] = mix(h[3], rrxs(vals[i], a, b, &c));
                h[3] = mix(h[3], c);
            }
        }
        line("rev16", h[0]); line("revsh", h[1]); line("rrx", h[2]);
        line("rrxs", h[3]);
    }
    {
        u32 h[4] = { 0 };
        for (int i = 0; i < NV; i++)
            for (int j = 0; j < NV; j++) {
                u32 d = vals[i], n = vals[j];
#define RUN(lsb, w) h[0] = mix(h[0], bfi_##lsb##_##w(d, n)); \
                h[1] = mix(h[1], bfc_##lsb##_##w(d)); \
                h[2] = mix(h[2], ubfx_##lsb##_##w(n)); \
                h[3] = mix(h[3], sbfx_##lsb##_##w(n));
                FIELDS(RUN)
            }
        line("bfi", h[0]); line("bfc", h[1]); line("ubfx", h[2]);
        line("sbfx", h[3]);
    }
    {
        u32 h[6] = { 0 };
        for (int k = 2; k <= 4; k += 2)
            for (int i = 0; i < NV; i++) {
                u32 a, b, mv;
                for (int z = 0; z < 8; z++)
                    buf[z] = vals[(i + z) % NV] ^ (u32)z;
                mv = ldrd_off(k, &a, &b);
                h[0] = mix(mix(mix(h[0], a), b), mv);
                mv = ldrd_pre(k, &a, &b);
                h[1] = mix(mix(mix(h[1], a), b), mv);
                mv = ldrd_post(k, &a, &b);
                h[2] = mix(mix(mix(h[2], a), b), mv);
                a = vals[(i + 5) % NV];
                b = vals[(i + 7) % NV];
                h[3] = mix(h[3], strd_off(k, a, b));
                h[4] = mix(h[4], strd_pre(k, b, a));
                h[5] = mix(h[5], strd_post(k, a ^ b, a));
                for (int z = 0; z < 8; z++)
                    h[3 + i % 3] = mix(h[3 + i % 3], buf[z]);
            }
        line("ldrd", h[0]); line("ldrd!", h[1]); line("ldrd post", h[2]);
        line("strd", h[3]); line("strd!", h[4]); line("strd post", h[5]);
    }
    {
        static unsigned char cb = 0xf0;
        static unsigned short ch = 0xfff0;
        u32 h[2] = { 0 };
        for (int i = 0; i < NV; i++) {
            h[0] = mix(mix(h[0], exb(&cb, vals[i])), cb);
            h[1] = mix(mix(h[1], exh(&ch, vals[i])), ch);
        }
        line("ldrexb/strexb", h[0]); line("ldrexh/strexh", h[1]);
#if defined(HOST) || !defined(__thumb__)
        {
            static u64 cd = 0xfffffffffffffff0ull;
            u32 hd = 0;
            for (int i = 0; i < NV; i++)
                hd = mix64(mix64(hd, exd(&cd, vals[i])), cd);
            line("ldrexd/strexd", hd);
        }
#endif
    }
    {
        const char *s = "==END==\n";
        while (*s)
            writec(*s++);
    }
    return 0;
}
