/* GNU C: arithmetic on a pointer to void or to a function steps by one
 * byte, and sizeof of void, of a function type or designator, and of a
 * void expression is 1 -- as GCC has them (EmbCC refused all of it). */
// expect-exit: 42
#include <stdio.h>
void g(void) { }
typedef void F(void);
_Static_assert(sizeof(void) == 1, "sizeof(void)");
int main(void)
{
    char buf[8] = "abcdefg";
    void *p = buf;
    void *q = p + 3;
    p++;
    p += 2;
    F *fp = g;
    F *fq = fp + 1;
    printf("%d %d %d %d %d %d\n", (int)(q - (void *)buf), (int)((char *)p - buf),
           (int)sizeof(void), (int)sizeof(g), (int)sizeof(g()),
           (int)((char *)fq - (char *)fp));
    return *(char *)q == 'd' && (char *)p - buf == 3 ? 42 : 1;
}
