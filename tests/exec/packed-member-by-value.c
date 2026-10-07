/* A packed structure's member passed and returned by value. Its address
 * need not be aligned to its type -- here it is one byte past a word --
 * so a backend whose word loads fault on a misaligned address (Xtensa,
 * MIPS) must not read the struct's words with them: the call's argument
 * words and a return's result words are read a byte at a time unless the
 * front end can promise the alignment (ir_arg.natural). */
// expect-exit: 42
struct in { int a, b; };
struct __attribute__((packed)) out { char c; struct in i; struct in j; };

__attribute__((noinline)) int take(struct in s) { return s.a * 10 + s.b; }
__attribute__((noinline)) int take3(int x, struct in s, int y)
{ return x + s.a * 100 + s.b * 10 + y; }
__attribute__((noinline)) struct in give(const struct out *p) { return p->j; }

struct out g = { 1, { 3, 4 }, { 5, 6 } };

int main(void)
{
    struct out *p = &g;
    struct out loc = { 2, { 7, 8 }, { 9, 1 } };
    struct in r;
    if (take(p->i) != 34) return 1;
    if (take3(1, p->j, 2) != 563) return 2;
    if (take(loc.i) != 78) return 3;
    r = give(p);
    if (r.a != 5 || r.b != 6) return 4;
    r = give(&loc);
    if (r.a != 9 || r.b != 1) return 5;
    return 42;
}
