/* Static data whose bytes depend on the byte order: compiled by EmbCC for
 * mips-none-elf and by clang for mips-unknown-elf, every object must have
 * the same bytes and the same relocations (tests/golden/mips-be-data.sh),
 * and the program prints the same values on the big-endian board.
 * No string literal is pointed at twice and every pointer names a global,
 * so the two objects' relocations can be compared by name. */
#include <stdint.h>

/* integers of every width, and their arrays */
unsigned char  d_u8   = 0xA5;
short          d_s16  = -2;
unsigned short d_u16  = 0xBEEF;
int            d_s32  = -123456789;
unsigned       d_u32  = 0xDEADBEEFu;
long           d_long = 0x01020304L;
long long      d_s64  = -0x0123456789ABCDEFLL;
unsigned long long d_u64 = 0xFEDCBA9876543210ULL;
_Bool          d_bool = 1;
enum colour { RED = 1, GREEN = 0x10203, BLUE = -7 } d_enum = GREEN;
short          d_sarr[5] = { 1, -1, 0x1234, -0x1234, 0x7fff };
unsigned       d_uarr[3] = { 0x11223344u, 0x55667788u, 0x99AABBCCu };
long long      d_llarr[2] = { 0x1122334455667788LL, -2LL };

/* floating point */
float          d_f    = -1.25f;
double         d_d    = 3.141592653589793;
long double    d_ld   = -0.1L;
double         d_darr[3] = { 1.0, -0.0, 1e-300 };
float          d_farr[2] = { 0.1f, 3.4e38f };
_Complex double d_cd  = 1.5 + 2.5i;
_Complex float  d_cf  = -0.5f + 4.0fi;

/* bit-fields: across a unit, of several types, signed, zero-width, in a
 * packed struct across its unit */
struct bf1 { unsigned a : 3, b : 5, c : 9, d : 15; };
struct bf2 { unsigned char x : 1; signed char y : 3; unsigned short z : 12;
             int w : 7; };
struct bf3 { int a : 4; int : 0; int b : 4; char c; unsigned d : 20; };
struct bf4 { long long a : 33; long long b : 31; };
struct __attribute__((packed)) bf5 { unsigned char p; unsigned q : 20;
                                     unsigned r : 12; };
struct bf6 { unsigned short s : 6; unsigned short t : 10; unsigned char u : 7; };
struct bf1 d_bf1 = { 5, 17, 300, 0x5A5A };
struct bf2 d_bf2 = { 1, -3, 0xABC, -50 };
struct bf3 d_bf3 = { -5, 7, 'Q', 0xFEDCB };
struct bf4 d_bf4 = { -0x123456789LL, 0x2A2A2A2A };
struct bf5 d_bf5 = { 0x11, 0xFEDCB, 0xA98 };
struct bf6 d_bf6 = { 33, 777, 99 };
union ubf { unsigned short h; struct { unsigned char lo : 4, hi : 4; } n; };
union ubf d_ubf = { .n = { 0xC, 0x3 } };

/* unions initialised through one member of several */
union u1 { uint32_t w; unsigned char b[4]; uint16_t h[2]; };
union u1 d_u1a = { 0x01020304u };
union u1 d_u1b = { .b = { 1, 2, 3, 4 } };
union u1 d_u1c = { .h = { 0xAABB, 0xCCDD } };
union u2 { double d; uint32_t w[2]; unsigned long long q; };
union u2 d_u2a = { 2.0 };
union u2 d_u2b = { .w = { 0x40090000u, 1 } };
union u2 d_u2c = { .q = 0x8000000000000001ULL };

/* aggregates mixing all of it, with padding */
struct mix { char c; short s; int i; long long ll; float f; double d;
             unsigned char tail[3]; };
struct mix d_mix = { 'x', -300, 0x7F00FF01, -1LL << 40, 2.5f, -8.0,
                     { 7, 8, 9 } };
struct nest { struct mix m[2]; union u1 u; struct bf1 b; };
struct nest d_nest = { { { 1, 2, 3, 4, 5.0f, 6.0, { 1 } },
                         { 'a', 'b', 'c', 'd', -1.0f, -2.0, { 2, 3, 4 } } },
                       { 0xCAFEBABEu }, { 1, 2, 3, 4 } };

/* strings, narrow and wide, as arrays */
char           d_str[]  = "byte order";
__CHAR16_TYPE__ d_u16s[] = u"\x0102\x0304\x00ff";
__CHAR32_TYPE__ d_u32s[] = U"\x01020304\x7fffffff";
__WCHAR_TYPE__ d_wstr[] = L"w\x12345";
const char     d_rostr[16] = "read-only";
const unsigned d_rotab[4] = { 0x1a2b3c4du, 0, 0xffffffffu, 12345 };
const double   d_rod = -1.0 / 3.0;

/* pointers with addends, and a narrow string literal pointed at */
int            d_target[8] = { 10, 11, 12, 13, 14, 15, 16, 17 };
int           *d_p0  = d_target;
int           *d_p3  = &d_target[3];
int           *d_pm  = &d_target[8];
char          *d_pc  = (char *)d_target + 5;
const char    *d_lit = "a literal";
struct mix    *d_pmx = &d_mix;
float         *d_pf  = &d_mix.f;
void          *d_ptab[3] = { &d_u8, &d_s64, (char *)&d_mix + 1 };
struct withp { int k; int *p; short s; } d_withp = { 3, &d_target[7], -9 };
uintptr_t      d_uptr = (uintptr_t)&d_target[2];

/* the board prints what the bytes mean */
extern void putn(long v);
extern void puts_(const char *s);
static void put64(unsigned long long v)
{
    putn((long)(unsigned)(v >> 32));
    putn((long)(unsigned)v);
}
static void putbytes(const void *p, unsigned n)
{
    const unsigned char *b = p;
    for (unsigned k = 0; k < n; k++)
        putn(b[k]);
    puts_("\n");
}
int main(void)
{
    put64(d_u8); put64((unsigned long long)d_s16); put64(d_u16);
    put64((unsigned long long)d_s32); put64(d_u32); put64((unsigned long long)d_long);
    put64((unsigned long long)d_s64); put64(d_u64); put64(d_bool); put64(d_enum);
    puts_("\n");
    putbytes(d_sarr, sizeof d_sarr); putbytes(d_uarr, sizeof d_uarr);
    putbytes(d_llarr, sizeof d_llarr);
    putbytes(&d_f, sizeof d_f); putbytes(&d_d, sizeof d_d);
    putbytes(&d_ld, sizeof d_ld); putbytes(d_darr, sizeof d_darr);
    putbytes(d_farr, sizeof d_farr); putbytes(&d_cd, sizeof d_cd);
    putbytes(&d_cf, sizeof d_cf);
    putn(d_bf1.a); putn(d_bf1.b); putn(d_bf1.c); putn(d_bf1.d);
    putn(d_bf2.x); putn(d_bf2.y); putn(d_bf2.z); putn(d_bf2.w);
    putn(d_bf3.a); putn(d_bf3.b); putn(d_bf3.c); putn((long)d_bf3.d);
    put64((unsigned long long)d_bf4.a); put64((unsigned long long)d_bf4.b);
    putn(d_bf5.p); putn((long)d_bf5.q); putn((long)d_bf5.r);
    putn(d_bf6.s); putn(d_bf6.t); putn(d_bf6.u);
    putn(d_ubf.h); putn(d_ubf.n.lo); putn(d_ubf.n.hi);
    puts_("\n");
    putbytes(&d_bf1, sizeof d_bf1); putbytes(&d_bf2, sizeof d_bf2);
    putbytes(&d_bf3, sizeof d_bf3); putbytes(&d_bf4, sizeof d_bf4);
    putbytes(&d_bf5, sizeof d_bf5); putbytes(&d_bf6, sizeof d_bf6);
    putn((long)d_u1a.w); putn(d_u1a.b[0]); putn(d_u1a.h[0]);
    putn((long)d_u1b.w); putn((long)d_u1c.w); putn(d_u1c.b[1]);
    put64(d_u2a.q); putn((long)d_u2a.w[0]); putn((long)d_u2b.w[1]);
    put64(d_u2b.q); putn((long)d_u2c.w[0]); putn((long)d_u2c.w[1]);
    puts_("\n");
    putbytes(&d_mix, sizeof d_mix); putbytes(&d_nest, sizeof d_nest);
    puts_(d_str); puts_("\n");
    putbytes(d_u16s, sizeof d_u16s); putbytes(d_u32s, sizeof d_u32s);
    putbytes(d_wstr, sizeof d_wstr); putn(d_u16s[1]); putn((long)d_u32s[1]);
    putn((long)d_wstr[1]); puts_(d_rostr); putbytes(d_rotab, sizeof d_rotab);
    putbytes(&d_rod, sizeof d_rod);
    putn(*d_p0); putn(*d_p3); putn(d_pm - d_p0); putn(*(unsigned char *)d_pc);
    puts_(d_lit); putn(d_pmx->s); putn((long)(*d_pf * 4));
    putn(*(unsigned char *)d_ptab[0]); put64((unsigned long long)*(long long *)d_ptab[1]);
    putn(*(unsigned char *)d_ptab[2]); putn(*d_withp.p); putn(d_withp.s);
    putn(*(int *)d_uptr);
    puts_("\n");
    return 0;
}
