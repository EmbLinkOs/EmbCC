/* GNU C: a cast to a union type, `(union u)x`, is the union whose member
 * of x's type holds x -- as an argument, a return value, an operand and
 * an initializer (static, automatic, nested) -- and is not an lvalue; a
 * struct cast to its own type is its value. */
// expect-exit: 42
#include <stdio.h>
union u { int i; float f; char *p; };
struct pt { int x, y; };
union w { double d; struct pt p; };
struct s { int a; char b; };

static int take(union u v) { return v.i; }
static float takef(union u v) { return v.f; }
static union u ret(int x) { return (union u)x; }

union u g1 = (union u)5;
union u g2 = (union u)1.5f;
struct box { char tag; union u v; struct pt q; } gb = { 'b', (union u)6, (struct pt){ 1, 2 } };
union u ga[2] = { (union u)3, (union u)0.5f };

int main(void)
{
    int ok = 0;
    int x = 7;
    const int cx = 9;
    float fl = 2.5f;
    char buf[] = "hi";
    struct pt p = { 3, 4 };
    struct s s1 = { 11, 'z' }, s2;
    ok += take((union u)x) == 7;
    ok += take((union u)cx) == 9;
    ok += takef((union u)fl) == 2.5f;
    ok += ((union u)buf).p == buf;
    ok += ((union w)p).p.y == 4;
    ok += ret(13).i == 13;
    union u v = (union u)(x + 1);
    ok += v.i == 8;
    s2 = (struct s)s1;
    ok += s2.a == 11 && s2.b == 'z';
    ok += g1.i == 5 && g2.f == 1.5f;
    static union u sl = (union u)17;
    struct box lb = { 'l', (union u)(x * 2), (struct pt)p };
    ok += sl.i == 17 && gb.v.i == 6 && gb.q.y == 2 && ga[0].i == 3 && ga[1].f == 0.5f;
    ok += lb.v.i == 14 && lb.q.x == 3 && lb.tag == 'l';
    printf("%d %d %g %d\n", v.i, ((union u)42).i, (double)((union u)fl).f, g1.i);
    return ok == 11 ? 42 : ok;
}
