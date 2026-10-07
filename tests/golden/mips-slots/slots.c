/* Delay-slot filling on MIPS32 (tests/golden/mips-slots.sh): each function
 * puts, right before a transfer, an instruction the filler must NOT move
 * into the slot -- or one it may -- and main checks the results.
 *
 *   operand:  the instruction writes what the branch compares
 *   link:     the instruction saves $ra, which jal has already
 *             overwritten when its slot runs
 *   target:   the instruction writes jalr's target register
 *   label:    a label between the instruction and the branch: a jump to
 *             the label must not run it
 *   args:     argument setup, which may go in a call's slot (and must
 *             still reach the callee)
 *   epilogue: two returns; the second's last instruction falls into the
 *             epilogue the first one branches to, so it must not go in
 *             the return's slot (the first path would run it too) */
void putn(long v);

__attribute__((noinline)) int id(int x) { return x; }
__attribute__((noinline)) int add(int a, int b) { return a + b; }

__attribute__((noinline)) int operand(int a, int b)
{
    int c = a + 1;
    if (c == b)                 /* addu c; beq c, b */
        return 10;
    return 20;
}

__attribute__((noinline)) int operand_lt(int a, int b)
{
    int c = a * 3;
    if (c < b)                  /* mul c; slt $at; bnez $at */
        return 1;
    return 2;
}

__attribute__((noinline)) int link(int x)
{
    /* the prologue's `sw $ra` comes right before this jal */
    return id(x) + 1;
}

__attribute__((noinline)) int target(int (*fp)(int), int x)
{
    return fp(x) * 2;           /* the target is loaded right before jalr */
}

__attribute__((noinline)) int label(int n)
{
    int s = 0;
    do {
        s += n;                 /* the loop's first instruction */
        n--;
    } while (n > 0);            /* a branch back to it */
    return s;
}

__attribute__((noinline)) int args(int a, int b, int c, int d)
{
    return add(add(a, b), add(c, d)) + add(d, 1);
}

__attribute__((noinline)) int epilogue(int a)
{
    if (a > 0)
        return a * 2;
    return -a;
}

static int (*volatile vfp)(int) = id;

int main(void)
{
    putn(operand(4, 5));        /* 10 */
    putn(operand(4, 6));        /* 20 */
    putn(operand_lt(2, 7));     /* 1 */
    putn(operand_lt(3, 7));     /* 2 */
    putn(link(41));             /* 42 */
    putn(target(vfp, 21));      /* 42 */
    putn(label(5));             /* 15 */
    putn(args(1, 2, 3, 4));     /* 15 */
    putn(epilogue(21));         /* 42 */
    putn(epilogue(-7));         /* 7 */
    return 42;
}
