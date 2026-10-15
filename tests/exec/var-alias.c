/* alias on variables: another name for a variable defined here, with no
 * storage of its own -- and to the optimizer the same object, so a store
 * through the alias is seen by a read of the target. */
// expect-exit: 42
int printf(const char *, ...);
int a = 40;
extern int b __attribute__((alias("a")));
static int c __attribute__((alias("a")));
const char tab[4] = "xyz";
extern const char tab2[4] __attribute__((weak, alias("tab")));
int z;
extern int zb __attribute__((alias("z")));
__attribute__((noinline)) static int f(void) { z = 1; zb = 2; return z; }
__attribute__((noinline)) static int g(int *p) { zb = 5; *p = 7; return zb; }
int main(void)
{
    b += 1;
    c += 1;
    int r = f(), s = g(&z);
    printf("%d %s %d %d\n", a, tab2, r, s);
    return a == 42 && &b == &a && tab2[1] == 'y' && r == 2 && s == 7 ? 42 : 1;
}
