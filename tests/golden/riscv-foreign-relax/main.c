/* The EmbCC half of tests/golden/riscv-foreign-relax.sh: calls into the
 * clang object and checks what comes back, and that the functions clang
 * asked to be aligned to 64 and 8 bytes are. */
int lib_classify(int x);
int lib_pre(int x);
int lib_vec(void);
int lib_after(int x);
int lib_sum(const int *p, int n);

int lib_ext(int x) { return x + 2; }

int main(void)
{
    static const int want[8] = { 11, 23, 35, 47, 59, 61, 73, -1 };
    static const int v[5] = { 3, -4, 5, -6, 7 };
    for (int k = 0; k < 8; k++)
        if (lib_classify(k) != want[k])
            return 1;
    if (((unsigned long)lib_vec & 63) != 0)
        return 2;
    if (((unsigned long)lib_after & 7) != 0)
        return 3;
    if (lib_vec() != 5 || lib_after(0) != 9 || lib_after(4) != 5)
        return 4;
    if (lib_sum(v, 5) != 25 || lib_pre(4) != 19)
        return 5;
    return 42;
}
