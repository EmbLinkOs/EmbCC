// GNU's label addresses in C++, as in C: &&label, a static table of them,
// and goto *expr -- the threaded-interpreter idiom.
// expect-exit: 42
static int run(const unsigned char *code)
{
    static void *const ops[] = { &&op_add, &&op_dbl, &&op_end };
    int acc = 0;
    goto *ops[*code];
op_add:
    acc += 1; code++;
    goto *ops[*code];
op_dbl:
    acc *= 2; code++;
    goto *ops[*code];
op_end:
    return acc;
}
int main()
{
    static const unsigned char prog[] = { 0, 0, 1, 1, 0, 1, 0, 2 };
    return run(prog) == 19 ? 42 : 1;   /* ((1+1)*2*2+1)*2+1 */
}
