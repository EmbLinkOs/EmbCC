// expect-exit: 42
/* __attribute__((aligned(N))) takes a constant EXPRESSION, as _Alignas
 * does. Only a bare number was read, and the rest of the argument
 * skipped: aligned(2*32) took the 2, aligned((64)) and
 * aligned(sizeof(long long)) the no-argument default of 16, and
 * aligned(1<<5) on a variable gave it an alignment of 1 -- every struct
 * layout and address that depended on them was silently wrong. */
enum { LINE = 32 };
struct a { char c; } __attribute__((aligned(2 * 32)));
struct b { char c; } __attribute__((aligned(sizeof(long long))));
struct c { char c; } __attribute__((aligned((64))));
struct d { char c __attribute__((aligned(4 * 2))); char e; };
struct e { int i; } __attribute__((aligned(LINE)));
char pad = 1;
char x __attribute__((aligned(1 << 5))) = 2;
char pad2 = 3;
char y __attribute__((aligned(4 * 4))) = 4;

int main(void)
{
    if (_Alignof(struct a) != 64 || sizeof(struct a) != 64) return 1;
    if (_Alignof(struct b) != sizeof(long long)) return 2;
    if (_Alignof(struct c) != 64) return 3;
    if (_Alignof(struct d) != 8 || sizeof(struct d) != 8) return 4;
    if (_Alignof(struct e) != 32) return 5;
    if ((unsigned long)&x % 32 != 0) return 6;
    if ((unsigned long)&y % 16 != 0) return 7;
    return pad + x + pad2 + y == 10 ? 42 : 8;
}
