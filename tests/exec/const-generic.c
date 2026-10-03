// expect-exit: 42
/* const is part of a type. It was not: a lone `int *` association took
 * a `const int *` operand, and a `const int *` association could never
 * match anything. Both chose a different expression from gcc and clang
 * without a word. */
#define KIND(p) _Generic((p), int *: 1, const int *: 2, \
                         char *: 3, const char *: 4, default: 0)

_Static_assert(KIND((const int *)0) == 2, "const int * is its own type");
_Static_assert(KIND((int *)0) == 1, "int * is not const int *");
_Static_assert(_Generic((const char *)0, char *: 1, default: 2) == 2,
               "a lone char * does not take const char *");

__attribute__((noinline)) static int kind_ci(const int *p) { return KIND(p); }
__attribute__((noinline)) static int kind_i(int *p) { return KIND(p); }
__attribute__((noinline)) static int kind_cs(const char *s) { return KIND(s); }

int main(void)
{
    int x = 0;
    const int cx = 5;
    /* lvalue conversion drops the operand's own const */
    if (_Generic(cx, int: 1, default: 0) != 1) return 1;
    if (kind_ci(&x) != 2 || kind_i(&x) != 1) return 2;
    if (kind_cs("s") != 4) return 3;
    /* a const object can be read, and a value made from it is plain */
    __auto_type y = cx;
    y = y + 1;
    __typeof__(cx + 1) z = 0;
    z = cx * 2;
    return y + z + 26 == 42 ? 42 : 4;
}
