/* Cases that go where the default goes are not tested by the dispatch --
 * those written beside `default:`, and, when there is no default, those
 * whose statement is the switch's own `break` -- and each value must still
 * land where C says. Every value from -2 to 12 is tried against each
 * shape, and against a reference written as if-chains. */
// expect-exit: 42

static int beside_default(int x)
{
    int r = 0;
    switch (x) {
    case 1: r = 10; break;
    case 2:
    case 3:
    default: r = 99; break;        /* 2 and 3 are the default */
    case 4: r = 40; break;
    }
    return r;
}

static int on_break_no_default(int x)
{
    int r = 5;
    switch (x) {
    case 0: case 1: case 2:
        break;                     /* the same as no case */
    case 3: r = 30; break;
    case 7: case 8: break;
    case 9: r = 90;                /* falls into the run below */
    case 10: case 11:
        break;
    }
    return r;
}

static int on_break_with_default(int x)
{
    int r = 5;
    switch (x) {
    case 0: case 1:
        break;                     /* NOT the default: it has its own */
    case 3: r = 30; break;
    default: r = 77;
    }
    return r;
}

static int at_the_end(int x)
{
    int r = 1;
    switch (x) {
    case 2: r = 20; break;
    case 5: case 6:                /* markers last: they break */
    }
    return r;
}

static int ref_beside(int x) { return x == 1 ? 10 : x == 4 ? 40 : 99; }
static int ref_nodef(int x) { return x == 3 ? 30 : x == 9 ? 90 : 5; }
static int ref_withdef(int x) { return x == 0 || x == 1 ? 5 : x == 3 ? 30 : 77; }
static int ref_end(int x) { return x == 2 ? 20 : 1; }

int main(void)
{
    for (volatile int x = -2; x <= 12; x++) {
        if (beside_default(x) != ref_beside(x))
            return 1;
        if (on_break_no_default(x) != ref_nodef(x))
            return 2;
        if (on_break_with_default(x) != ref_withdef(x))
            return 3;
        if (at_the_end(x) != ref_end(x))
            return 4;
    }
    return 42;
}
