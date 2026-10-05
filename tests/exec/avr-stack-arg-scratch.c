/* A call with a 64-bit argument in registers and another on the stack,
 * where the first one's value is already in its argument registers when
 * the second is stored -- here it is the result of a call, which AVR
 * returns in r18-r25, exactly the registers the first argument goes in.
 * The stack argument was copied through r18 up, over it, and the callee
 * received the stack argument's bytes twice. (Seen first at -O0, where
 * `-5` is computed rather than folded; a call result does it at every
 * level.) Harmless elsewhere: on targets that pass all of these in
 * registers, nothing goes on the stack. */
// expect-exit: 42
__attribute__((noinline)) static long long minus5(void) { return -5; }

__attribute__((noinline)) static long long stk(long long a, long b, long c,
                                               long long d)
{
    return a * 1000 + b * 100 + c * 10 + (d & 0xff);
}

int main(void)
{
    if (stk(minus5(), 7L, 9L, 0x0123456789ABCDEFLL) != -5000 + 700 + 90 + 0xef)
        return 1;
    return 42;
}
