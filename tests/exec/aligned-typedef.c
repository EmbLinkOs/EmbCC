/* __attribute__((aligned(N))) on a typedef: the type's alignment is N,
 * larger or smaller than its own, and its size does not change (GCC's and
 * clang's rule). EmbCC refused a larger one and ignored a smaller one --
 * so a struct holding the smaller had another layout than GCC's. The
 * smaller is the idiom for an unaligned access: through such a type the
 * address promises only N, and a core that traps on a misaligned word
 * (Cortex-M0, SPARC, MIPS) must be given byte accesses. */
// expect-exit: 42
#include <stddef.h>
#include <string.h>

typedef unsigned A8 __attribute__((aligned(8)));
typedef unsigned U1 __attribute__((aligned(1)));      /* unaligned access */
typedef unsigned short H1 __attribute__((aligned(1)));
typedef unsigned char Buf[6] __attribute__((aligned(16)));
typedef struct { char c; } S16 __attribute__((aligned(16)));
typedef __attribute__((aligned(4))) char C4;           /* the leading form */

struct M { char c; A8 x; };                 /* x at 8, size 16 */
struct U { char c; U1 x; };                 /* x at 1, size 5 */
struct B { char c; Buf b; };                /* b at 16 */

_Static_assert(sizeof(A8) == sizeof(unsigned) && _Alignof(A8) == 8, "A8");
_Static_assert(sizeof(U1) == sizeof(unsigned) && _Alignof(U1) == 1, "U1");
_Static_assert(sizeof(Buf) == 6 && _Alignof(Buf) == 16, "Buf");
_Static_assert(sizeof(S16) == 1 && _Alignof(S16) == 16, "S16 keeps its size");
_Static_assert(_Alignof(C4) == 4, "C4");
_Static_assert(offsetof(struct M, x) == 8 && sizeof(struct M) == 16, "M");
_Static_assert(offsetof(struct U, x) == 1 && _Alignof(struct U) == 1, "U");
_Static_assert(offsetof(struct B, b) == 16, "B");

Buf gbuf;                                   /* a DMA buffer's alignment */
static char pad1;
static Buf gbuf2;
static volatile int vz;

/* writes and reads through U1/H1 at every offset of a byte buffer: each
 * address is only byte-aligned for three of the four */
__attribute__((noinline)) static unsigned roundtrip(unsigned char *p, int off,
                                                    unsigned v)
{
    U1 *q = (U1 *)(p + off);
    H1 *h = (H1 *)(p + off + 4);
    *q = v - 1;
    *q += 1;                                /* a read-modify-write too */
    *h = (unsigned short)(v >> 3) - 1;
    (*h)++;
    return *q ^ *h;
}

/* parameters of these types: their slots are the calling convention's,
 * and big-endian targets write an incoming register as a whole word */
__attribute__((noinline)) static unsigned params(A8 x, U1 y, H1 z)
{
    A8 *px = &x;
    return *px * 3 + y + z;
}

__attribute__((noinline)) static unsigned member(struct U *u)
{
    return u->x;                            /* at offset 1 */
}

int main(void)
{
    unsigned char raw[32];
    pad1 = 1;
    if ((size_t)gbuf % 16 || (size_t)gbuf2 % 16)
        return 1;
    {
        Buf local;                          /* a local is placed so too */
        char c = 0;
        A8 l8 = 3;
        if ((size_t)local % 16 || (size_t)&l8 % 8)
            return 2;
        local[0] = (unsigned char)c;
        (void)local;
    }
    for (int off = vz; off < 8; off++) {
        unsigned v = 0x89abcdefu + (unsigned)off;
        memset(raw, 0, sizeof raw);
        if (roundtrip(raw, off, v) != (v ^ (unsigned short)(v >> 3)))
            return 3;
        /* the bytes in memory are the host's own order, at off */
        unsigned w;
        memcpy(&w, raw + off, sizeof w);
        if (w != v)
            return 4;
    }
    if (params(5, 7, 11) != 33)
        return 7;
    {
        struct U u;
        u.c = 7;
        u.x = 0x01020304u;
        if (member(&u) != 0x01020304u || u.c != 7)
            return 5;
    }
    {
        typedef int L8 __attribute__((aligned(8)));   /* block scope */
        L8 a = 5;
        if (_Alignof(L8) != 8 || (size_t)&a % 8 || a != 5)
            return 6;
    }
    return 42;
}
