/* A return statement's expression is a full expression, the comma
 * operator's too (C11 6.8.6.4): `return *p = 3, 4;` stores and returns 4.
 * EmbCC read only an assignment-expression there and refused the comma;
 * EmbSim's MIPS disassembler is written that way. */
// expect-exit: 42
static int store_then(int *p)
{
    return *p = 3, 4;
}

static int last_of(int a, int b)
{
    return a++, b += a, a + b;          /* 3, b 8: 11 */
}

int main(void)
{
    int x = 0;
    int r = store_then(&x);             /* r 4, x 3 */
    if (r != 4 || x != 3)
        return 1;
    if (last_of(2, 5) != 11)
        return 2;
    return r * 10 + x - 1;              /* 42 */
}
