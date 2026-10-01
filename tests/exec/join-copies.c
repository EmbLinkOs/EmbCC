/* The copies leaving SSA puts at a join, coalesced in the IR: `%b = mov
 * %a` takes %a's name away when the copy is %a's one reader and the two
 * never hold different live values at once. Each loop here carries a
 * value through such copies in a shape where one of those conditions
 * FAILS, so a coalescing that skipped it computes something else:
 *   - the old value still read after the new one is computed (b live
 *     right after a's definition);
 *   - the merge target rewritten while the source is on its way to it
 *     (a live right after another definition of b) -- a value computed
 *     before a loop and copied into a merge whose other arm writes it;
 *   - the source read somewhere besides the copy;
 *   - swaps and three-way rotations, where every copy interferes;
 * and the shapes it is FOR -- nested choices assigning one variable, a
 * state machine -- at int, long long (a register pair on the 32-bit
 * targets) and double. Expectations are closed forms or volatile
 * references the optimizer cannot share code with. */
// expect-exit: 42
static volatile int vn = 37, vx = 5;

__attribute__((noinline)) int old_read(int x, int n)
{ int b = x, s = 0; for (int i = 0; i < n; i++) { int a = b * 3 + i; s += b; b = a; } return s + b; }
__attribute__((noinline)) int pending(int x, int n)
{ int b = 0, s = 0; for (int i = 0; i < n; i++) { int a = x + i; if (i & 1) b = i; s += b; b = a; } return s + b; }
static volatile int vsink;
/* the source computed once, before the loop, and copied into the merge
 * on one arm while the other arm writes the merge itself: the source is
 * pending across that write */
__attribute__((noinline)) int pending2(int x, int n)
{ int a = x * 3 + 1, s = 0; for (int i = 0; i < n; i++) { int b; if (i % 3) b = a; else { b = 5; vsink = i; } s += b * (i + 1); } return s; }
__attribute__((noinline)) int two_readers(int x, int n)
{ int b = x, s = 0; for (int i = 0; i < n; i++) { int a = b + i; s += a * 2; b = a; } return s + b; }
__attribute__((noinline)) long long fib(int n)
{ long long a = 0, b = 1; for (int i = 0; i < n; i++) { long long t = a + b; a = b; b = t; } return a; }
__attribute__((noinline)) int rot3(int n)
{ int x = 1, y = 2, z = 3; for (int i = 0; i < n; i++) { int t = x; x = y; y = z; z = t + x; } return x * 100 + y * 10 + z; }
__attribute__((noinline)) double dswap(int n)
{ double a = 1.5, b = -2.25; for (int i = 0; i < n; i++) { double t = a; a = b * 0.5; b = t + 1.0; } return a + b; }
__attribute__((noinline)) int nested(int c, int d, int e)
{ int st = c ? (d ? (e ? 1 : 2) : 3) : (e ? 4 : 5); return st; }
__attribute__((noinline)) long long nested64(int c, int d, long long base)
{ long long v = c ? (d ? base + 1 : base * 3) : (d ? base - 7 : base << 2); return v; }
__attribute__((noinline)) int machine(const char *p)
{
    int st = 0, n = 0;
    for (; *p; p++) {
        char c = *p;
        switch (st) {
        case 0: st = c >= '0' && c <= '9' ? 1 : c == '-' ? 2 : 5; break;
        case 1: st = c >= '0' && c <= '9' ? 1 : c == '.' ? 3 : 5; break;
        case 2: st = c >= '0' && c <= '9' ? 1 : 5; break;
        case 3: st = c >= '0' && c <= '9' ? 4 : 5; break;
        case 4: st = c >= '0' && c <= '9' ? 4 : 5; break;
        default: n++; break;
        }
    }
    return st * 1000 + n;
}

int main(void)
{
    int n = vn, x = vx;
    /* references: the same loops over volatile state */
    { volatile int b = x, s = 0; for (int i = 0; i < n; i++) { int a = b * 3 + i; s += b; b = a; } if (old_read(x, n) != s + b) return 1; }
    { volatile int b = 0, s = 0; for (int i = 0; i < n; i++) { int a = x + i; if (i & 1) b = i; s += b; b = a; } if (pending(x, n) != s + b) return 2; }
    { volatile int b = x, s = 0; for (int i = 0; i < n; i++) { int a = b + i; s += a * 2; b = a; } if (two_readers(x, n) != s + b) return 3; }
    { volatile int s = 0, va = x * 3 + 1; for (int i = 0; i < n; i++) s += (i % 3 ? va : 5) * (i + 1); if (pending2(x, n) != s) return 10; }
    if (fib(n) != 24157817LL || fib(1) != 1 || fib(0) != 0) return 4;
    { volatile int a = 1, b = 2, c = 3; for (int i = 0; i < n; i++) { int t = a; a = b; b = c; c = t + a; } if (rot3(n) != a * 100 + b * 10 + c) return 5; }
    { volatile double a = 1.5, b = -2.25; for (int i = 0; i < n; i++) { double t = a; a = b * 0.5; b = t + 1.0; } if (dswap(n) != a + b) return 6; }
    static const int want[8] = { 5, 4, 5, 4, 3, 3, 2, 1 };   /* c d e as bits 2 1 0 */
    for (int k = 0; k < 8; k++) if (nested(k >> 2 & 1, k >> 1 & 1, k & 1) != want[k]) return 7;
    if (nested64(1, 1, 1LL << 40) != (1LL << 40) + 1 || nested64(1, 0, 1LL << 40) != 3LL << 40 ||
        nested64(0, 1, 1LL << 40) != (1LL << 40) - 7 || nested64(0, 0, 1LL << 40) != 1LL << 42) return 8;
    if (machine("123") != 1000 || machine("-12.5") != 4000 || machine("1.2.3") != 5001 ||
        machine("x--") != 5002 || machine("") != 0 || machine("-") != 2000) return 9;
    return 42;
}
