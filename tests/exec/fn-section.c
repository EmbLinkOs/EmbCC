/* __attribute__((section("s"))) on a function puts its code in section s
 * -- a routine that must run from RAM while flash is written, a hot
 * path, a boot stage -- and EmbCC refused it ("every function is
 * emitted into .text"). The functions here sit in two sections of their
 * own and in .text, and call each other across all three, directly and
 * through a table: a call that crosses a section is the linker's to
 * resolve, one within a section is not. */
// expect-exit: 42
static int base(int x) { return x * 2; }
__attribute__((section(".text.hot"))) int hot(int x) { return base(x) + 1; }
__attribute__((section(".ramfunc"))) static int ram2(int x) { return x - 3; }
__attribute__((section(".ramfunc"))) int ram(int x) { return hot(x) + ram2(x); }
__attribute__((section(".text.hot"))) int hot2(int x) { return ram(x) + hot(1); }
int (*const table[])(int) = { hot, ram, hot2 };
int tail(int x) { return x + 1; }
int main(void)
{
    int s = 0;
    for (int i = 0; i < 3; i++)
        s += table[i](2);          /* hot 5, ram 5 + -1 = 4, hot2 4 + 3 = 7 */
    return s + hot2(1) + tail(23) - 2;   /* 16 + 4 + 24 - 2 */
}
