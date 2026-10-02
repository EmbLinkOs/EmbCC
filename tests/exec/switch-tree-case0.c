/* A switch lowered as a compare tree (every switch at -Os below six
 * cases; any whose values are too sparse for a table), inside a loop,
 * whose selector the optimizer can prove is 0. Two cfg-clean rules met
 * on the tree's `brnz c; brnz c -> L9; jmp L4; L9:` -- the second branch
 * condemned as unable to fire, then inverted into `brz c -> L4` before
 * the condemned instruction was removed -- and the way to case 0 (L4)
 * went with it. SCCP deleted the case as unreachable: f(0) returned -1.
 * Found by the native Darwin fuzzer; exec-Os.sh runs this at -Os. */
// expect-exit: 42
__attribute__((noinline)) static int f(int a, int k)
{
    int l = -1;
    for (unsigned i = 0; i < 2; i++) {
        switch (((unsigned)a * 8u) & 7) {       /* always 0 */
        case 0:
            l = k;
        case 1: case 2: case 6: case 7:
            ;
        }
    }
    return l;
}
__attribute__((noinline)) static int g(unsigned v)
{
    int r = 0;
    for (int i = 0; i < 3; i++) {
        switch (v & 15) {
        case 0: r += 1;
        case 3: r += 10; break;
        case 9: r += 100; break;
        case 12: r += 1000; break;
        case 14: r += 10000;
        }
    }
    return r;
}
int main(void)
{
    int bad = 0;
    if (f(0, 7) != 7) bad |= 1;
    if (f(5, 9) != 9) bad |= 2;
    if (g(0) != 33) bad |= 4;
    if (g(9) != 300) bad |= 8;
    if (g(14) != 30000) bad |= 16;
    if (g(5) != 0) bad |= 32;
    return bad ? bad : 42;
}
