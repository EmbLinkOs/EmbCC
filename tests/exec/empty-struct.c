// GNU C's structure and union with no members: size 0, alignment 1,
// usable everywhere a complete type is -- as a member (taking no room),
// an array element, a global, a local, a parameter between others, a
// return value, in an assignment and a constant expression. A pointer to
// one moves by 0. Printed, so tests/golden/agrees-with-gcc.sh holds it to
// GCC's.
// expect-exit: 42
#include <stddef.h>
#include <stdio.h>

struct E { };
union U { };
struct W { struct E a, b; union U u; };
struct O { char a; struct E e; int b; struct E e3[3]; char c; };

struct E ge;
struct E gea[4];
struct O go = { .a = 1, .b = 2, .c = 3 };
struct E *gp = &gea[3];

_Static_assert(sizeof(struct E) == 0, "an empty struct has size 0");
_Static_assert(_Alignof(struct E) == 1, "and alignment 1");
_Static_assert(sizeof(struct W) == 0 && sizeof gea == 0, "so do its arrays");

static int calls;

static struct E make(void)
{
    struct E r;
    calls++;
    return r;
}

static struct E put(int *p, struct E e, int v)
{
    *p = v;
    return e;
}

static int mid(int a, struct E e, int b)
{
    (void)e;
    return a * 10 + b;
}

static long many(int a, struct E e1, long b, union U u, double c,
                 struct E e2, int d)
{
    (void)e1; (void)u; (void)e2;
    return a + b + (long)c + d;
}

int main(void)
{
    struct E x, y = make();
    struct W w;
    struct O o = go;
    struct E *p = &gea[1];

    x = y;
    x = make();
    w.a = x;
    int got = 0;
    y = put(&got, put(&calls, x, 2), 5);
    printf("%d %d %d %d %d\n", (int)sizeof(struct E), (int)sizeof(union U),
           (int)sizeof(struct W), (int)sizeof w, (int)_Alignof(struct W));
    printf("O %d b@%d e3@%d c@%d\n", (int)sizeof(struct O),
           (int)offsetof(struct O, b), (int)offsetof(struct O, e3),
           (int)offsetof(struct O, c));
    printf("%d %d %d\n", o.a, o.b, o.c);
    printf("%d %d %d\n", p + 3 == p, (char *)&o.e3[2] == (char *)&o.c,
           gp == gea);
    printf("%d %ld\n", mid(4, x, 2), many(1, x, 2, w.u, 3.5, y, 4));
    return calls == 2 && got == 5 && o.a + o.b + o.c == 6 ? mid(4, ge, 2) : 1;
}
