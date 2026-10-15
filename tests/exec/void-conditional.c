/* ?: with a void arm: GNU C's one-void-arm form, and two void arms
 * where one is a void call (whose merge read an undefined temp at -O2). */
// expect-exit: 42
int printf(const char *, ...);
static int n;
static void bump(void) { n++; }
struct s { int a; } sv = { 5 };
int main(void)
{
    int k = 3;
    k ? bump() : 1;
    0 ? bump() : (void)(n += 10);
    k ? (void)0 : (void)sv;
    (k > 5) ? 7 : bump();
    printf("%d\n", n);
    return n == 12 ? 42 : n;
}
