// expect-exit: 42
/* A loop whose body ends in several places -- the cases of a switch --
 * sends each end to one block that copies the loop's carried values and
 * jumps back. Each case now does those copies itself and jumps straight
 * to the head. This runs a small register machine whose loop carries the
 * program counter and three registers, each changed by a different case,
 * and compares the answer with a fixed one. */
enum { SETA, ADDA, MULB, DECC, JNZC, HALT };
static const int prog[] = {
    SETA, 1,
    /* 2: */ MULB, ADDA, 3, DECC, JNZC, 2,
    HALT,
};

__attribute__((noinline)) static long run(long c)
{
    long a = 0, b = 1;
    int pc = 0;
    for (;;) {
        int op = prog[pc++];
        switch (op) {
        case SETA: a = prog[pc++]; break;
        case ADDA: a += prog[pc++]; break;
        case MULB: b *= a; break;
        case DECC: c--; break;
        case JNZC: { int t = prog[pc++]; if (c) pc = t; break; }
        case HALT: return b * 100 + a;
        default:   return -1;
        }
    }
}

int main(void)
{
    /* a = 1, 4, 7, 10, 13; b = 1*1*4*7*10*13 = 3640; a ends at 16 */
    return run(5) == 3640 * 100 + 16 ? 42 : 1;
}
