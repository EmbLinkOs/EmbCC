/* C23: a definition may leave a parameter unnamed -- the callback that
 * ignores its context pointer, `void on_tick(void *) { ... }`. GCC 15 and
 * clang accept it by default, and EmbCC refused it ("parameter 1 of
 * 'on_tick' needs a name in a definition"). The parameter still takes its
 * argument's place: the named ones after it must read the right values,
 * on every target's convention, and the address taken of the function
 * must be callable through a pointer of the full type.
 */
// expect-exit: 42
typedef void (*cb_t)(void *, int);

static int hits;

static void on_tick(void *, int n) { hits += n; }

static int second(int, int b) { return b; }

static long long mixed(char, long long w, double, int k)
{
    return w + k;
}

int main(void)
{
    cb_t c = on_tick;
    c(0, 30);
    return hits + second(7, 2) + (int)mixed('x', 6, 1.5, 4);
}
