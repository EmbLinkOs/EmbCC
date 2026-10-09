/* cov.c -- the program tests/golden/embsim-coverage.sh holds EmbSim's
 * --coverage to. Built at -O0, where each statement is its own code, so
 * which lines run, and how often, is known exactly: every line that has
 * code ends in a mark, `=N` for a line that runs N times and `=0` for
 * one that never runs; a line with no mark has no code. The test reads
 * the marks and compares them with the report, line for line.
 *
 * What it exercises: a branch taken and one not taken (classify's three
 * ways, and main's `if` whose body never runs), a loop (five turns, its
 * test six times), a function never called (never_called), and one
 * called from a loop (classify, five times). Each function returns from
 * one place: a line's count is its most-executed instruction's, and
 * -O0's shared epilogue would count every return on the last one's
 * line. */
void putn(long v);

static int never_called(int x)                                  /* =0 */
{
    return x * 3;                                               /* =0 */
}

static int classify(int v)                                      /* =5 */
{
    int r = 1;                                                  /* =5 */
    if (v < 0)                                                  /* =5 */
        r = -1;                                                 /* =1 */
    else if (v == 0)                                            /* =4 */
        r = 0;                                                  /* =1 */
    return r;                                                   /* =5 */
}

int main(void)                                                  /* =1 */
{
    int sum = 0;                                                /* =1 */
    for (int i = 0; i < 5; i++)                                 /* =6 */
        sum += classify(i - 1);                                 /* =5 */
    if (sum > 100)                                              /* =1 */
        sum = never_called(sum);                                /* =0 */
    putn(sum);                                                  /* =1 */
    return 0;                                                   /* =1 */
}
