/* Small structs copied whole -- between locals, through pointers, out of
 * arrays and variadic arguments, inside a loop -- which the optimizer now
 * copies field by field (pass_aggcopy) so that the locals can leave memory
 * (SROA, which follows an address at the target's pointer width: it
 * followed 8-byte adds alone, so on a 32-bit or AVR target no field past
 * the first was ever split). Padding, a nested struct, a pointer field, a
 * packed struct (left a memcpy), and a struct whose address escapes after
 * a copy.
 */
// expect-exit: 42
#include <stdarg.h>

struct pt { short x, y; };
struct rec { char tag; int val; struct pt at; };      /* padded */
struct node { struct node *next; unsigned id; };
struct __attribute__((packed)) pk { char c; int i; };

static volatile int vone = 1;

__attribute__((noinline)) static struct rec make(int k)
{
    struct rec r;
    r.tag = (char)('a' + k);
    r.val = k * 100 * vone;
    r.at.x = (short)-k;
    r.at.y = (short)(k + 1);
    return r;
}

__attribute__((noinline)) static int sum_va(int n, ...)
{
    va_list ap;
    int s = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        struct pt p = va_arg(ap, struct pt);
        struct pt q = p;                    /* a copy of the copy */
        s += q.x * 10 + q.y;
    }
    va_end(ap);
    return s;
}

__attribute__((noinline)) static void escape(struct pt *p) { p->y += 1; }

int main(void)
{
    int r = 0;
    struct rec a = make(2), b;
    b = a;                                  /* local to local */
    b.val += 1;
    if (a.val == 200 && b.val == 201 && b.tag == 'c' && b.at.x == -2 &&
        b.at.y == 3)
        r += 10;

    struct rec arr[3] = { make(0), make(1), make(3) };
    struct rec best = arr[0];
    for (int i = 1; i < 3; i++)             /* copies inside a loop */
        if (arr[i].val > best.val)
            best = arr[i];
    if (best.tag == 'd' && best.val == 300)
        r += 8;

    struct node n2 = { 0, 7 }, n1 = { &n2, 5 }, c;
    c = *n1.next;                           /* through a pointer */
    if (c.id == 7 && c.next == 0)
        r += 6;

    struct pt p1 = { 1, 2 }, p2 = { 3, 4 };
    if (sum_va(2, p1, p2) == 12 + 34)
        r += 6;

    struct pt e = p2;                       /* copied, then escapes */
    escape(&e);
    if (e.y == 5 && p2.y == 4)
        r += 6;

    struct pk k1 = { 'z', 0x12345678 }, k2;
    k2 = k1;                                /* packed: a memcpy still */
    if (k2.i == 0x12345678 && k2.c == 'z')
        r += 6;
    return r;
}
