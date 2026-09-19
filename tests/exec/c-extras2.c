/* C99/GNU C that libiberty's demangler and libgcc's unwind-pe.h use:
 * flexible array members (and GNU's [0]) — sized and laid out as gcc
 * does, an `aligned` one too; enumerator values that are constant
 * expressions; a comma expression as if/while/for condition; a form feed
 * between tokens. */
// expect-exit: 42
#include <stddef.h>

struct Buf { short n; double d[]; };
struct Aligned { int n; char data[] __attribute__((aligned(16))); };
struct Zero { int n; long none[0]; };

enum { SHIFT = 3, FLAG = 1 << SHIFT, BOTH = FLAG | 2, NEG = -BOTH,
       NEXT, SIZED = sizeof(int) * 2 };

static int next_digit(const char **p) { return *(*p)++ - '0'; }

int main(void)
{
    static double storage[4];              /* room for a Buf and 3 more */
    struct Buf *b = (struct Buf *)storage;
    b->n = 3;
    for (int i = 0; i < b->n; i++)
        b->d[i] = i * 1.5;
    int ok = sizeof(struct Buf) == 8 && offsetof(struct Buf, d) == 8 &&
             sizeof(struct Aligned) == 16 &&
             offsetof(struct Aligned, data) == 16 &&
             sizeof(struct Zero) == 8 && b->d[2] == 3.0;
    ok = ok && FLAG == 8 && BOTH == 10 && NEG == -10 && NEXT == -9 &&
         SIZED == 8;
    const char *s = "1234";
    int sum = 0, d;
    while (d = next_digit(&s), d < 4)
        sum += d;
    if (sum += 100, sum != 106)
        ok = 0;
    for (d = 0; d++, d < 5;)
        sum++;
	ok = ok && sum == 110;
	return ok ? 42 : 1;
}
/* (a form feed below) */

