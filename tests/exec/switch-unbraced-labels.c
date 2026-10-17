/* A switch's body is any statement, a labelled one included:
 * `switch (x) case 4: case 0: r = 10;` (GCC's pr126405.c). A case label
 * is a marker in the body's list, so with no braces the body was the
 * labels alone and the statement they label ran after the switch, for
 * every x -- and a second label said "'case' outside of a switch". */
// expect-exit: 42

__attribute__((noinline)) static int f(int x)
{
    int r = 0;
    switch (x)
        case 4:
        case 0:
            r = 10;
    switch (x)
        default:
            r += 1;
    return r;
}

int main(void)
{
    return f(4) == 11 && f(0) == 11 && f(2) == 1 ? 42 : 1;
}
