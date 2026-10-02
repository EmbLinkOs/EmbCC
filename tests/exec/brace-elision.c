/* Brace elision (C11 6.7.9p20): a subaggregate initialized without
 * braces of its own takes as many initializers from the enclosing list
 * as it holds. Every elided form here is compared with its fully braced
 * spelling, and the unsized arrays' element counts with what C gives. */
// expect-exit: 42
#include <string.h>
struct pt { int x, y; };
struct in { int a, b; };
struct o1 { struct in in; int c; };
struct o2 { char name[8]; int x; };
struct nest { struct { int a[2]; int b; } in[2]; int z; };
struct un { union { int i; char c[4]; } u; int t; };
struct bf { unsigned a : 3, b : 5; };
struct cz { double _Complex z; int k; };
enum { A = 1, B };

/* static storage: lowered to bytes through the same walk */
static struct pt g1[] = { 1, 2, 3, 4, 5 };              /* 3 elements */
static const struct pt g1r[] = { {1, 2}, {3, 4}, {5} };
static struct nest g2 = { 1, 2, 3, 4, 5, 6, 7 };
static const struct nest g2r = { { { {1, 2}, 3 }, { {4, 5}, 6 } }, 7 };
static struct pt g3[] = { A, 10, B, 20 };               /* enumerators: 2 */
static int g4[][3] = { 1, 2, 3, 4 };                    /* 2 rows */
static struct o2 g5[] = { "ab", 1, "cd", 2 };           /* 2 */
static char g6[] = { "abc" };                           /* 4 bytes */
static struct bf g7[] = { 1, 2, 3, 4 };                 /* 2 */
static double _Complex g8[] = { 1, 2.0 + 3.0i, 4 };     /* a scalar: 3 */

#define SAME(p, q) (sizeof(p) == sizeof(q) && memcmp(&(p), &(q), sizeof(p)) == 0)

int main(void)
{
    int bad = 0;
    if (!SAME(g1, g1r)) bad |= 1 << 0;
    if (!SAME(g2, g2r)) bad |= 1 << 1;
    if (sizeof g3 != 2 * sizeof(struct pt) || g3[1].x != 2 || g3[1].y != 20) bad |= 1 << 2;
    if (sizeof g4 != 6 * sizeof(int) || g4[1][0] != 4 || g4[1][1] != 0) bad |= 1 << 3;
    if (sizeof g5 != 2 * sizeof(struct o2) || strcmp(g5[1].name, "cd") || g5[1].x != 2) bad |= 1 << 4;
    if (sizeof g6 != 4 || strcmp(g6, "abc")) bad |= 1 << 5;
    if (sizeof g7 != 2 * sizeof(struct bf) || g7[1].a != 3 || g7[1].b != 4) bad |= 1 << 6;

    /* the `= {0}` idiom, whatever comes first */
    struct o1 z1 = {0};
    struct o2 z2 = {0};
    struct pt z3[4] = {0};
    struct nest z4 = {0};
    if (z1.in.a | z1.in.b | z1.c | z2.name[0] | z2.x | z3[3].y | z4.in[1].a[1] | z4.z) bad |= 1 << 7;

    struct o1 a = { 1, 2, 3 };
    struct o1 ar = { {1, 2}, 3 };
    if (!SAME(a, ar)) bad |= 1 << 8;
    int m[2][3] = { 1, 2, 3, 4 };
    int mr[2][3] = { {1, 2, 3}, {4} };
    if (!SAME(m, mr)) bad |= 1 << 9;
    /* a struct value fills the whole member; braces resume elision */
    struct in iv = { 7, 8 };
    struct o1 b = { iv, 9 };
    if (b.in.a != 7 || b.in.b != 8 || b.c != 9) bad |= 1 << 10;
    struct pt arr[] = { iv.a, iv.b, { 5, 6 }, 7 };         /* 3 elements */
    struct pt arrr[] = { {7, 8}, {5, 6}, {7} };
    if (!SAME(arr, arrr)) bad |= 1 << 11;
    /* iv.a might have been a struct value as far as the syntax goes, so
     * no size is folded before the types settle it: k has 3 elements */
    int k[sizeof arr / sizeof arr[0]];
    if (sizeof k != 3 * sizeof(int)) bad |= 1 << 11;
    struct pt sv[] = { {1, 2}, {3, 4} };
    struct pt whole[] = { sv[1], sv[0] };                   /* values: 2 */
    if (sizeof whole != 2 * sizeof(struct pt) || whole[0].x != 3 || whole[1].y != 2) bad |= 1 << 12;
    /* designators end an elided brace and name the braced list's member */
    struct o1 d = { 1, 2, .c = 9 };
    struct pt dp[3] = { 1, 2, [2] = 5, 6 };
    struct pt dpr[3] = { {1, 2}, {0}, {5, 6} };
    struct pt dq[] = { [1] = 3, 4, 5 };                     /* 3 elements */
    struct pt dqr[] = { {0}, {3, 4}, {5} };
    if (d.in.a != 1 || d.in.b != 2 || d.c != 9 || !SAME(dp, dpr) || !SAME(dq, dqr)) bad |= 1 << 13;
    /* a union takes its first member */
    struct un u1 = { 1, 2 };
    if (u1.u.i != 1 || u1.t != 2) bad |= 1 << 14;
    /* a compound literal sizes itself the same way */
    struct pt *cl = (struct pt[]){ 1, 2, 3, 4 };
    struct pt *cl2 = (struct pt[]){ 1, 2, 3 };
    if (cl2[1].x != 3 || cl2[1].y != 0) bad |= 1 << 15;
    if (cl[1].x != 3 || cl[1].y != 4) bad |= 1 << 16;
    /* static locals */
    static struct o1 sl = { 4, 5, 6 };
    if (sl.in.a != 4 || sl.in.b != 5 || sl.c != 6) bad |= 1 << 17;
    char s4[4] = { "abc" };
    char s5[] = { "abcd" };
    if (strcmp(s4, "abc") || sizeof s5 != 5) bad |= 1 << 18;
    /* braces apply to the subobject that is current: in[1], not in[1].a */
    struct nest n = { 1, 2, 3, { 4, 5 }, 6 };
    struct nest nr = { { { {1, 2}, 3 }, { {4, 5}, 0 } }, 6 };
    if (!SAME(n, nr)) bad |= 1 << 19;
    /* a complex number takes one initializer, as any scalar: 4 is 4+0i */
    double _Complex t[3] = { 1, 2.0 + 3.0i, 4 };
    struct cz cs = { 5, 6 };
    struct cz csa[2] = { 7, 8, 9, 10 };
    if (__real__ t[0] != 1 || __imag__ t[0] != 0 || __imag__ t[1] != 3 || __real__ t[2] != 4 ||
        __real__ cs.z != 5 || __imag__ cs.z != 0 || cs.k != 6 ||
        __real__ csa[1].z != 9 || csa[1].k != 10 || csa[0].k != 8 ||
        sizeof g8 != 3 * sizeof(double _Complex) || __real__ g8[2] != 4 || __imag__ g8[1] != 3)
        bad |= 1 << 20;
    return bad ? (bad & 0x7f) + 1 + (bad >> 20) * 2 : 42;
}
