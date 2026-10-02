/* A function declared in a block -- `extern int f(int);` or plain
 * `int f(int);` -- is in scope from there to the block's end: a call
 * reaches it even when the definition comes later in the file, and the
 * declaration hides an outer variable of the same name. EmbCC refused
 * the first ("call before its declaration") and took the second as a
 * variable. */
// expect-exit: 42
struct h { int a, b; };
static int twice(int x) { return 2 * x; }
int use(struct h x)
{
    extern int later(struct h);           /* defined below */
    int r = later(x);                     /* 40 */
    {
        int inner(int);                   /* no `extern`: still a function */
        r += inner(1);                    /* +1 */
    }
    {
        extern int zero(int);
        int (*q)(int) = zero;             /* a later function as a value */
        r += q(7);                        /* +0 */
    }
    return r;
}
int shadow(void)
{
    int (*pick)(int) = twice;             /* a local pointer named pick */
    int s = pick(3);                      /* twice: 6 */
    {
        int pick(int);                    /* hides it: the function below */
        s += pick(5);                     /* 100 */
        int (*p)(int) = pick;
        s += p(5) - 100;                  /* the function again: +0 */
    }
    s += pick(4);                         /* the pointer again: 8 */
    return s;
}
int later(struct h x) { return x.a * x.b; }
int inner(int v) { return v; }
int zero(int v) { return v - v; }
int pick(int v) { return 100 + 0 * v; }
int main(void)
{
    struct h v = { 5, 8 };
    return use(v) + 1 == 42 && shadow() == 114 ? 42 : 1;
}
