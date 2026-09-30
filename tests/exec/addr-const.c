/* Address constants built on an INTEGER, the way every microcontroller
 * header names its registers: `&GPIOA->ODR` in a static const pointer.
 * C11 6.6p9 makes these constant expressions -- a cast of an integer to a
 * pointer, then &, *, ->, . and [] over it -- and EmbCC refused all but the
 * bare cast, and folded pointer ARITHMETIC in a static initializer as if
 * it counted bytes: `(unsigned *)0x100 + 1` was 0x101. Each static value
 * is compared with the same expression computed at run time, which irgen
 * lowers and scales on its own path. Nothing is dereferenced. */
// expect-exit: 42
typedef unsigned long uptr;
struct regs { unsigned a, b; unsigned arr[4]; };

volatile unsigned *const p1 = (volatile unsigned *)0x84;
volatile unsigned *const p2 = &*(volatile unsigned *)0x84;
volatile unsigned char *const p3 = &((volatile unsigned char *)0x80)[4];
volatile unsigned *const p4 = &((struct regs *)0x100)->b;
volatile unsigned *const p5 = &((struct regs *)0x100)->arr[2];
unsigned *const p6 = (unsigned *)0x100 + 1;
unsigned *const p7 = (unsigned *)0x100 - 2;
struct regs *const p8 = (struct regs *)0x200 + 2;
volatile unsigned *const p9 = &((struct regs *)0x100)[1].b;
long off = (long)&((struct regs *)0)->arr[1];
long diff = (unsigned *)0x110 - (unsigned *)0x100;

__attribute__((noinline)) static uptr at(uptr base) { return base; }

int main(void)
{
    struct regs *r = (struct regs *)at(0x100);
    if ((uptr)p1 != 0x84 || (uptr)p2 != 0x84 || (uptr)p3 != 0x84) return 1;
    if (p4 != &r->b) return 2;
    if (p5 != &r->arr[2]) return 3;
    if (p6 != (unsigned *)at(0x100) + 1) return 4;
    if (p7 != (unsigned *)at(0x100) - 2) return 5;
    if (p8 != (struct regs *)at(0x200) + 2) return 6;
    if (p9 != &r[1].b) return 7;
    if (off != (long)((uptr)&r->arr[1] - (uptr)r)) return 8;
    if (diff != (long)(0x10 / sizeof(unsigned))) return 9;
    return 42;
}
