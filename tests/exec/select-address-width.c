/* A select between two addresses is pointer-wide. if-convert takes the
 * select's width from what defines its arms, and irgen leaves an
 * address's w at its default 4: once value numbering had made both arms
 * of `c ? "dot" : "none"` plain moves of string addresses computed
 * earlier, the select was 32 bits, and on x86-64 and aarch64 the pointer
 * came back without its top half (embsvd, built by EmbCC at -O2, crashed
 * in sprintf). The same for a global's, a function's and a local's
 * address. */
// expect-exit: 42
static volatile int sink;
static int g1, g2;

__attribute__((noinline)) static void use(const void *p) { sink += p != 0; }
__attribute__((noinline)) static int f1(void) { return 1; }
__attribute__((noinline)) static int f2(void) { return 2; }

__attribute__((noinline)) static const char *pick_str(int c)
{
    use("dot");
    use("none");
    return c ? "dot" : "none";
}

__attribute__((noinline)) static int *pick_glob(int c)
{
    use(&g1);
    use(&g2);
    return c ? &g1 : &g2;
}

__attribute__((noinline)) static int (*pick_fn(int c))(void)
{
    use((const void *)f1);
    use((const void *)f2);
    return c ? f1 : f2;
}

__attribute__((noinline)) static int pick_local(int c)
{
    int a = 5, b = 7;
    use(&a);
    use(&b);
    int *p = c ? &a : &b;
    return *p;
}

int main(void)
{
    if (pick_str(1)[0] != 'd' || pick_str(0)[0] != 'n') return 1;
    if (pick_glob(1) != &g1 || pick_glob(0) != &g2) return 2;
    if (pick_fn(1)() != 1 || pick_fn(0)() != 2) return 3;
    if (pick_local(1) != 5 || pick_local(0) != 7) return 4;
    return 42;
}
