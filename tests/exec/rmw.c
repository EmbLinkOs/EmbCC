/* Read-modify-write: `x = *p; *p = x OP v` is one x86 instruction, `OP v,
 * (p)`, when nothing between the load and the store can write *p. Every
 * op that has the form (+ - & | ^), at 32 and 64 bits, from a register
 * and from an immediate (one byte, four bytes, and one too wide for the
 * 64-bit form), on globals, on a local array (an address off the frame),
 * through pointers, and on struct fields -- and the shapes that must NOT
 * be fused: the loaded value on the right of a subtraction, a volatile,
 * a store through another pointer that may be the same one, and a call,
 * between the load and the store. Each result is checked against the same
 * computation done one step at a time through a volatile, which nothing
 * fuses. */
// expect-exit: 42
typedef unsigned long long u64;

static int gi[16];
static long long gl[16];
struct rec { int n; long long total; unsigned flags; };
static volatile int vk = 3;

__attribute__((noinline)) void bump(int *count, const unsigned char *s, int n)
{ for (int i = 0; i < n; i++) count[s[i] & 15]++; }

__attribute__((noinline)) void ops32(int *a, int v)
{ a[0] += v; a[1] -= v; a[2] &= v; a[3] |= v; a[4] ^= v;
  a[5] += 100; a[6] -= 1000; a[7] &= 0x0ff0; a[8] |= 0x10000; a[9] ^= -1; }

__attribute__((noinline)) void ops64(long long *a, long long v)
{ a[0] += v; a[1] -= v; a[2] &= v; a[3] |= v; a[4] ^= v;
  a[5] += 100; a[6] -= 100000; a[7] &= 0x7fffffffLL; a[8] |= 0x123456789LL;
  a[9] ^= -2; a[10] += 0x100000000LL; a[11] -= -2147483648LL; }

__attribute__((noinline)) int local_array(int n)
{
    int a[8];
    for (int i = 0; i < 8; i++) a[i] = i;
    for (int i = 0; i < n; i++) { a[i & 7] += i; a[(i + 3) & 7] ^= i * 5; a[(i + 5) & 7] -= 2; }
    int s = 0;
    for (int i = 0; i < 8; i++) s = s * 7 + a[i];
    return s;
}

__attribute__((noinline)) void fields(struct rec *r, int k)
{ r->n += k; r->total -= k; r->flags |= 1u << (k & 31); r->flags &= ~4u; }

/* must stay three instructions */
__attribute__((noinline)) int rsub(int *p, int v) { int x = *p; *p = v - x; return *p; }
__attribute__((noinline)) int vol(volatile int *p, int v) { int x = *p; *p = x + v; return *p; }
__attribute__((noinline)) int alias(int *p, int *q, int v) { int x = *p; *q = 5; *p = x + v; return *p; }
static int side;
__attribute__((noinline)) int inc_side(void) { side += 7; return 1; }
__attribute__((noinline)) int across_call(int *p) { int x = *p; int y = inc_side(); *p = x + y; return *p; }
__attribute__((noinline)) int self_call(void) { int x = side; int y = inc_side(); side = x + y; return side; }

static int ref32(int a, int v, int which)
{
    volatile int x = a;
    switch (which) {
    case 0: x = x + v; break; case 1: x = x - v; break; case 2: x = x & v; break;
    case 3: x = x | v; break; case 4: x = x ^ v; break; case 5: x = x + 100; break;
    case 6: x = x - 1000; break; case 7: x = x & 0x0ff0; break;
    case 8: x = x | 0x10000; break; default: x = x ^ -1; break;
    }
    return x;
}

int main(void)
{
    int bad = 0;
    unsigned char s[300];
    unsigned x = 7;
    for (int i = 0; i < 300; i++) { x = x * 1103515245u + 12345u; s[i] = (unsigned char)(x >> 16); }
    int count[16] = { 0 }, ref[16] = { 0 };
    bump(count, s, 300);
    for (int i = 0; i < 300; i++) ref[s[i] & 15]++;
    for (int i = 0; i < 16; i++) if (count[i] != ref[i]) bad |= 1;

    for (int i = 0; i < 16; i++) gi[i] = 0x5a5a5a5a ^ (i * 0x01010101);
    int before[16];
    for (int i = 0; i < 16; i++) before[i] = gi[i];
    ops32(gi, 0x12345 + vk);
    for (int i = 0; i < 10; i++) if (gi[i] != ref32(before[i], 0x12345 + vk, i)) bad |= 2;
    for (int i = 10; i < 16; i++) if (gi[i] != before[i]) bad |= 4;

    for (int i = 0; i < 16; i++) gl[i] = (long long)0x5a5a5a5a5a5a5a5aULL ^ ((long long)i << 40);
    long long lb[16];
    for (int i = 0; i < 16; i++) lb[i] = gl[i];
    long long lv = 0x123456789abLL + vk;
    ops64(gl, lv);
    long long want[12] = { lb[0] + lv, lb[1] - lv, lb[2] & lv, lb[3] | lv, lb[4] ^ lv,
                           lb[5] + 100, lb[6] - 100000, lb[7] & 0x7fffffffLL,
                           lb[8] | 0x123456789LL, lb[9] ^ -2, lb[10] + 0x100000000LL,
                           (long long)((u64)lb[11] + 2147483648ULL) };
    for (int i = 0; i < 12; i++) if (gl[i] != want[i]) bad |= 8;
    for (int i = 12; i < 16; i++) if (gl[i] != lb[i]) bad |= 16;

    {
        int a[8];
        for (int i = 0; i < 8; i++) a[i] = i;
        int n = 50 + vk;
        for (int i = 0; i < n; i++) {
            volatile int t;
            t = a[i & 7]; a[i & 7] = t + i;
            t = a[(i + 3) & 7]; a[(i + 3) & 7] = t ^ (i * 5);
            t = a[(i + 5) & 7]; a[(i + 5) & 7] = t - 2;
        }
        int sref = 0;
        for (int i = 0; i < 8; i++) sref = sref * 7 + a[i];
        if (local_array(n) != sref) bad |= 32;
    }

    struct rec r = { 10, 1000, 0xff };
    fields(&r, 9 + vk);
    if (r.n != 22 || r.total != 988 || r.flags != ((0xffu | (1u << 12)) & ~4u)) bad |= 64;

    int c = 10;
    if (rsub(&c, 3) != -7 || c != -7) bad |= 128;
    volatile int vv = 5;
    if (vol(&vv, 4) != 9) bad |= 256;
    int p1 = 100, p2 = 200;
    if (alias(&p1, &p2, 1) != 101 || p2 != 5) bad |= 512;
    if (alias(&p1, &p1, 1) != 102) bad |= 1024;              /* *q is *p: 101 + 1, not 5 + 1 */
    side = 0;
    int cc = 40;
    if (across_call(&cc) != 41 || side != 7) bad |= 2048;
    side = 10;
    if (self_call() != 11) bad |= 4096;                      /* x read before the call: 10 + 1 */

    return bad ? 100 + (bad % 150) : 42;
}
