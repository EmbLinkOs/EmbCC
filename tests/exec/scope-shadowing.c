/* Which variable a name means, as blocks open and close: sema finds a name
 * through an index of the visible variable of each name, and a closing
 * block gives each of its names back to the one it hid. Every case below
 * is one where a wrong answer reads another variable or none -- the first
 * version of the index lost a name reused by a sibling block ("'p' is not
 * declared"). */
// expect-exit: 42

int g = 100;

static int sibling_blocks(void)
{
    int s = 0;
    { int p = 1; s += p; }
    { int p = 2; s += p; }             /* the same name, a new block */
    { int p = 3; { int p = 4; s += p; } s += p; }
    return s;                          /* 1 + 2 + 4 + 3 */
}

static int shadow_and_restore(void)
{
    int x = 10, r = 0;
    {
        int x = 20;
        r += x;
        {
            int x = 30;
            r += x;
        }
        r += x;                        /* 20 again */
    }
    r += x;                            /* 10 again */
    return r;                          /* 20 + 30 + 20 + 10 */
}

static int for_scopes(void)
{
    int i = 1000, s = 0;
    for (int i = 0; i < 3; i++)
        s += i;
    for (int i = 5; i < 7; i++)
        s += i;
    return s + i;                      /* 0+1+2 + 5+6 + 1000 */
}

static int global_hidden(void)
{
    int r = g;                         /* 100 */
    {
        int g = 7;
        r += g;
    }
    {
        extern int g;                  /* the global again, in a block */
        r += g;
    }
    return r + g;                      /* 100 + 7 + 100 + 100 */
}

int main(void)
{
    if (sibling_blocks() != 10)
        return 1;
    if (shadow_and_restore() != 80)
        return 2;
    if (for_scopes() != 1014)
        return 3;
    if (global_hidden() != 307)
        return 4;
    return 42;
}
