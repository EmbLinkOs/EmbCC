/* ovf.c -- the program tests/golden/embsim-stack.sh overflows the stack
 * with: deep() recurses DEPTH times, each frame with a 64-byte buffer,
 * and prints what it adds up. Deep enough, it runs the stack below its
 * limit, or into .bss and .data. */
void putn(long v);

volatile int sink;

__attribute__((noinline)) int deep(int n)
{
    volatile char buf[64];
    buf[0] = (char)n;
    sink = buf[0];
    if (n == 0)
        return 0;
    return deep(n - 1) + (buf[0] & 1);
}

int main(void)
{
    putn(deep(DEPTH));
    return 0;
}
