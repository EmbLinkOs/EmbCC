/* `&&label` of a label no path reaches, in a function with no computed
 * goto. The address still has to be one: the label has to be placed.
 * SCCP's reachability started at the entry and followed edges only, so
 * it dropped the block the label opens, and the backend stopped on the
 * `&&L` naming it ("label 2 in 'f' was never placed", at -O1 and up on
 * every target). From GCC's torture suite: compile/labels-1.c,
 * labels-2.c (the address in a struct's initializer) and 20021108-1.c
 * (a difference of two, one after the return). Nothing here jumps to
 * one, so only what C promises of an address is asserted: it is not
 * null. */
// expect-exit: 42

struct bp { void *v, *b, *e; };
volatile long sink;

static int labels1(void)
{
    void *x = &&L2;
    if (&&L3 - &&L1 > 1000000)
        return 1;
L1: return x ? 10 : 2;
L2: return 3;
L3:;
    return 4;
}

static int labels2(void)
{
    struct bp x = { &&L2, 0, 0 };
    sink = &&L3 - &&L1;
L1: return x.v ? 20 : 5;
L2: return 6;
L3:;
    return 7;
}

static long diff(void)
{
l1:
    return &&l1 - &&l2;
l2:;
    return 0;
}

/* in a static initializer: the label is named by the data, not the code */
static int table(void)
{
    static void *const tab[] = { &&dead };
    if (!tab[0])
        return 8;
    return 12;
dead:
    return 9;
}

int main(void)
{
    sink = diff();
    return labels1() + labels2() + table();
}
