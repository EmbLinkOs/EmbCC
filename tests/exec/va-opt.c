/* C23's __VA_OPT__(content): the content when a variadic macro's
 * variable arguments expand to something, nothing when they do not --
 * the comma of `printf(f __VA_OPT__(,) __VA_ARGS__)`. EmbCC stopped at
 * "expected ')' before '__VA_OPT__'". Nested parentheses in the content,
 * an argument that expands to nothing, and a paste with it. */
// expect-exit: 42
static int sum(int n, ...) { return n; }
static int count(int a, ...) { return a; }
#define F(fmt, ...) sum(fmt __VA_OPT__(,) __VA_ARGS__)
#define G(...) count(10 __VA_OPT__(+ (__VA_ARGS__) * 2))
#define EMPTY
#define H(x, ...) x __VA_OPT__(+ 100)
#define CAT(a, ...) a ## __VA_OPT__(_tail)
static int one_tail = 7, one = 1;
#define STR(...) #__VA_ARGS__
int main(void)
{
    int bad = 0;
    if (F(5) != 5 || F(6, 1, 2) != 6) bad |= 1;
    if (G() != 10 || G(3) != 16 || G((1, 2)) != 14) bad |= 2;
    if (H(1, EMPTY) != 1 || H(1, 2) != 101) bad |= 4;
    if (CAT(one) != 1 || CAT(one, x) != 7) bad |= 8;
    return bad ? bad : 42;
}
