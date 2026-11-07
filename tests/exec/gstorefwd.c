/* Constants stored to a global, forwarded to its loads across blocks
 * (src/opt/memfwd.c, pass_gstorefwd): a table filled by an unrolled loop
 * and read after another loop that writes something else gives the reads
 * the constants. And each thing that must stop it: a call that rewrites
 * the table, a store through a pointer that may point into it, a store at
 * a variable index -- each read after it must see what it wrote. */
// expect-exit: 42
static short tbl[16], other[64];
static volatile int sink;

__attribute__((noinline)) void clobber(int k)
{
    tbl[k] = 1000;
}

/* macros, so the fill and the reads are in the function being tested */
#define fill() do { for (int i_ = 0; i_ < 16; i_++) \
                        tbl[i_] = (short)(i_ * 1234 % 8000 - 4000); } while (0)
#define SUM(s) do { s = 0; for (int k_ = 0; k_ < 16; k_++) \
                          s += tbl[k_] * (k_ + 1); } while (0)

static int ref_sum(int clobbered, short val)
{
    int s = 0;
    for (int k = 0; k < 16; k++) {
        short v = (short)(k * 1234 % 8000 - 4000);
        if (k == clobbered) v = val;
        s += v * (k + 1);
    }
    return s;
}

/* forwarded: the loop between writes only `other` */
__attribute__((noinline)) int fwd(int n)
{
    fill();
    for (int i = 0; i < 64; i++)
        other[i] = (short)(i * n);
    int s;
    SUM(s);
    return s + other[n & 63];
}

/* a call that writes the table */
__attribute__((noinline)) int through_call(int k)
{
    fill();
    clobber(k);
    int s;
    SUM(s);
    return s;
}

/* a pointer that may point into it */
__attribute__((noinline)) int through_ptr(short *p)
{
    fill();
    *p = 77;
    int s;
    SUM(s);
    return s;
}

/* a variable index */
__attribute__((noinline)) int var_index(int k)
{
    fill();
    tbl[k] = -5;
    int s;
    SUM(s);
    return s;
}

int main(void)
{
    volatile int three = 3, five = 5, nine = 9;
    if (fwd(three) != ref_sum(-1, 0) + 9) return 1;
    if (through_call(five) != ref_sum(5, 1000)) return 2;
    if (through_ptr(&tbl[nine]) != ref_sum(9, 77)) return 3;
    if (through_ptr(&other[nine]) != ref_sum(-1, 0)) return 4;
    if (var_index(three) != ref_sum(3, -5)) return 5;
    sink = other[0];
    return 42;
}
