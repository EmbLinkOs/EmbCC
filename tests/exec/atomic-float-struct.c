/* _Atomic beyond integers and pointers, which EmbCC refused ("EmbCC makes
 * the operators atomic for integers and pointers only"): a float and a
 * double -- read, assigned, `+=`, `*=`, `/=`, `++` and `--`, each a
 * compare-and-swap loop on the bits -- and structs: a 4-byte and an
 * 8-byte one, which _Atomic aligns to their size as GCC does and moves
 * in one access (or libatomic's 8-byte call on a 32-bit target), and a
 * 12-byte and a 3-byte one, which go through libatomic's generic
 * __atomic_load and __atomic_store. A struct's value is a copy: writing
 * the copy leaves the object as it was. */
// expect-exit: 42
#include <stdatomic.h>
#include <stddef.h>

/* GCC's rule: an _Atomic struct of a power-of-two size is aligned to that
 * size, or to the most the target aligns anything to */
#define MAXAL _Alignof(max_align_t)
#define WANT(n) ((n) < MAXAL ? (n) : MAXAL)

struct c4 { char a, b, c, d; };
struct s8 { short a, b, c, d; };
struct s12 { int a, b, c; };
struct s3 { char a, b, c; };

_Atomic float af = 1.5f;
_Atomic double ad = 2.0;
_Atomic struct c4 ac4;
_Atomic struct s8 as8;
_Atomic struct s12 as12;
_Atomic struct s3 as3;

__attribute__((noinline)) static float bump(void) { return ++af; }

int main(void)
{
    if (af != 1.5f) return 1;
    af += 2.25f;                         /* 3.75 */
    af *= 2.0f;                          /* 7.5 */
    if (af != 7.5f) return 2;
    float post = af--;                   /* 7.5, then 6.5 */
    if (post != 7.5f || af != 6.5f) return 3;
    if (bump() != 7.5f) return 4;
    ad /= 4.0;                           /* 0.5 */
    ad = ad + 1.25;                      /* 1.75 */
    ad++;
    if (ad != 2.75) return 5;

    if (_Alignof(_Atomic struct c4) != WANT(4) ||
        _Alignof(_Atomic struct s8) != WANT(8))
        return 6;
    struct c4 c = { 1, 2, 3, 4 };
    ac4 = c;
    struct c4 r = ac4;
    if (r.a != 1 || r.d != 4) return 7;
    r.b = 99;                            /* a copy: the object keeps 2 */
    struct c4 r2 = ac4;
    if (r2.b != 2) return 8;
    struct s8 s = { 10, 20, 30, 40 };
    as8 = s;
    struct s8 s2 = as8;
    if (s2.c != 30) return 9;
    struct s12 t = { 7, 8, 9 };
    as12 = t;
    struct s12 u = as12;
    if (u.a != 7 || u.c != 9) return 10;
    struct s3 v = { 5, 6, 7 };
    as3 = v;
    struct s3 v2 = as3;
    if (v2.b != 6) return 11;
    return 42;
}
