/* Switch threading in a function that falls off its closing brace. The
 * loop's test is then the function's last block, `brnz c -> loop` with
 * the implicit return as its other way out -- a way out the CFG lists
 * no successor for. The pass took the block for a latch (one way out),
 * and copied it without the branch; and took it into the path to the
 * switch, whose copy branches the other way to the block after it, which
 * does not exist: it read past the end of its label array and branched
 * to label 0 ("label 0 in 'f' was never placed" at -O2 on x86-64 and
 * the M4, whatever the garbage named elsewhere; a heap overrun, SIGBUS
 * or SIGSEGV, when a later pass indexed by it). From GCC's torture
 * suite: compile/920624-1.c (f, g) and compile/pr83510.c (test_3). */
// expect-exit: 42

int B[8], Q[8];

void f(void)
{
    int s;
    for (s = 0; s <= 6; s++)
        switch (s) {
        case 2: case 3: ++B[s];
        case 4: case 5: ++Q[s];
        }
}

/* the torture file's own: no case is ever taken */
void g(void)
{
    int s;
    for (s = 0; s <= 1; s++)
        switch (s) {
        case 2: case 3: ++B[s];
        case 4: case 5: ++Q[s];
        }
}

struct xyz { unsigned a0; };
unsigned arr[10];
static unsigned calls, last;

__attribute__((noinline)) void wfm(struct xyz *c, int i, unsigned v)
{
    (void)c;
    calls++;
    last = (unsigned)i * 100 + v;
}

__attribute__((noinline)) int get_flag(void) { return 1; }

/* case 9 is the only one the loop reaches */
void test_3(struct xyz *ctx)
{
    unsigned int i;
    for (i = 0; i < 10; i++) {
        switch (i) {
        case 0x9:
            wfm(ctx, i, ctx->a0);
            break;
        case 0xA: case 0xB:
        case 0xC: case 0xD: case 0xE: case 0xF:
        case 0x10: case 0x11: case 0x12: case 0x13:
            if (get_flag())
                wfm(ctx, i, arr[i - 0xa]);
            break;
        }
    }
}

int main(void)
{
    struct xyz x = { 7 };
    f();
    g();
    test_3(&x);
    int sum = 0;
    for (int i = 0; i < 8; i++)
        sum += B[i] * (i + 1) + Q[i] * 10;
    /* B[2] B[3]: 3 + 4; Q[2..5]: 40 */
    return sum == 47 && calls == 1 && last == 907 ? 42 : sum;
}
