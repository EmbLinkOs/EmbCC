/* Loads of a static global nothing writes fold to its initializer
 * (opt pass_roload) -- and only those. Each static below is either
 * provably read-only, or written, escaped or pointed at in a way the
 * fold must notice; a fold of the wrong kind returns the initializer
 * where the program changed the value, and the sum comes out wrong.
 */
// expect-exit: 42
static const double half = 0.5;                 /* folded: a double's bits */
static const int table[4] = { 3, 5, 7, 11 };    /* folded at constant index */
static const short parts[2] = { -2, 300 };      /* a signed narrow read */
static int bumped = 10;                         /* written through a pointer */
static int passed = 20;                         /* its address escapes */
static int pointed = 30;                        /* another initializer points at it */
static int *const aim = &pointed;
static long long wide = 0x100000002LL;          /* read as its low word */
static int direct = 5;                          /* assigned directly */
static int cells[3] = { 1, 2, 3 };              /* an element assigned */

__attribute__((noinline)) static void poke(int *p) { *p += 1; }

int main(void)
{
    int *bp = &bumped;
    *bp = 11;                       /* bumped is 11, not its initializer */
    poke(&passed);                  /* passed is 21 */
    *aim = 31;                      /* pointed is 31 */
    direct = 6;                     /* a store to its own address */
    cells[2] = 9;
    int r = 0;
    r += (int)(half * 4.0);         /* 2 */
    r += table[1] + table[3];       /* 16 */
    r += parts[0] + (parts[1] > 256);   /* -2 + 1 */
    r += bumped - 10;               /* 1 */
    r += passed - 20;               /* 1 */
    r += pointed - 30;              /* 1 */
    r += *(int *)&wide;             /* 2, little-endian low word */
    r += direct - 6;                /* 0 -- or -1 if folded to 5 */
    r += cells[2] - 9;              /* 0 -- or -6 if folded to 3 */
    return r + 20;                  /* 2+16-1+1+1+1+2 = 22, +20 = 42 */
}
