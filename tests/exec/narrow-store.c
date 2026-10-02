/* A 64-bit value stored into a narrower object: `*(unsigned *)p =
 * (unsigned)(v >> i)`, a struct's int field, a short array element, a
 * byte, a four-byte local. On a 32-bit target the value is a register
 * pair and the store must write only the low `size` bytes of it; RV32
 * wrote all eight, so the four bytes past the object -- the next field,
 * the next element, a neighbouring local, or past the frame's top the
 * caller's frame -- took the value's high word. And the other way: a
 * narrow object read as a 64-bit value reads only its own bytes and
 * extends. Every neighbour is checked after every store, at a shift the
 * optimizer cannot see, signed and unsigned. */
// expect-exit: 42
typedef unsigned long long u64;
typedef long long s64;
static volatile int vshift = 3;
static volatile u64 vv = 0xA5A5A5A5DEADBEEFULL;

struct rec { unsigned lo; unsigned guard1; unsigned short h; unsigned short guard2; unsigned char b; unsigned char guard3[3]; };

__attribute__((noinline)) void put32(unsigned *p, u64 v, int i) { *p = (unsigned)(v >> i); }
__attribute__((noinline)) void put16(unsigned short *p, u64 v, int i) { *p = (unsigned short)(v >> i); }
__attribute__((noinline)) void put8(unsigned char *p, u64 v, int i) { *p = (unsigned char)(v >> i); }
__attribute__((noinline)) void puts32(int *p, s64 v, int i) { *p = (int)(v >> i); }
__attribute__((noinline)) void fill(struct rec *r, u64 v, int i)
{ r->lo = (unsigned)(v >> i); r->h = (unsigned short)(v >> i); r->b = (unsigned char)(v >> i); }

/* locals: the truncating store lands in a 4-byte slot with neighbours */
__attribute__((noinline)) unsigned local_mix(u64 v, int n)
{
    unsigned a = 0x11111111u, u, c = 0x33333333u;
    unsigned short s;
    unsigned sum = 0;
    for (int i = 0; i < n; i++) {
        u = (unsigned)(v >> i);
        s = (unsigned short)(v >> (i + 1));
        sum += u + s + a + c;
    }
    return sum;
}
/* narrow reads widened to 64 */
__attribute__((noinline)) s64 rd_s32(const int *p) { s64 x = *p; return x; }
__attribute__((noinline)) u64 rd_u32(const unsigned *p) { u64 x = *p; return x; }
__attribute__((noinline)) s64 rd_s16(const short *p) { s64 x = *p; return x; }
__attribute__((noinline)) s64 rd_local(int k) { int a = k; short b = (short)k; s64 x = a; s64 y = b; return x + y; }

int main(void)
{
    int sh = vshift;
    u64 v = vv;
    unsigned w[3] = { 0xCAFEF00Du, 0, 0xCAFEF00Du };
    put32(&w[1], v, sh);
    if (w[1] != (unsigned)(v >> sh) || w[0] != 0xCAFEF00Du || w[2] != 0xCAFEF00Du) return 1;
    unsigned short hw[3] = { 0xBEEF, 0, 0xBEEF };
    put16(&hw[1], v, sh);
    if (hw[1] != (unsigned short)(v >> sh) || hw[0] != 0xBEEF || hw[2] != 0xBEEF) return 2;
    unsigned char bw[3] = { 0x5A, 0, 0x5A };
    put8(&bw[1], v, sh);
    if (bw[1] != (unsigned char)(v >> sh) || bw[0] != 0x5A || bw[2] != 0x5A) return 3;
    int iw[3] = { -7, 0, -7 };
    puts32(&iw[1], -(s64)v, sh);
    if (iw[1] != (int)(-(s64)v >> sh) || iw[0] != -7 || iw[2] != -7) return 4;
    struct rec r = { 0, 0x77777777u, 0, 0x7777, 0, { 0x77, 0x77, 0x77 } };
    fill(&r, v, sh);
    if (r.lo != (unsigned)(v >> sh) || r.h != (unsigned short)(v >> sh) || r.b != (unsigned char)(v >> sh)) return 5;
    if (r.guard1 != 0x77777777u || r.guard2 != 0x7777 || r.guard3[0] != 0x77 || r.guard3[2] != 0x77) return 6;
    unsigned want = 0;
    for (int i = 0; i < 9; i++)
        want += (unsigned)(v >> i) + (unsigned short)(v >> (i + 1)) + 0x11111111u + 0x33333333u;
    if (local_mix(v, 9) != want) return 7;
    int si[2] = { -5, 0x7EADBEEF };
    unsigned ui[2] = { 0xFFFFFFFBu, 0x7EADBEEF };
    short ss[2] = { -3, 0x7EAD };
    if (rd_s32(si) != -5 || rd_u32(ui) != 0xFFFFFFFBULL || rd_s16(ss) != -3) return 8;
    if (rd_local(-9) != -18 || rd_local(0x12345) != 0x12345 + 0x2345) return 9;
    return 42;
}
