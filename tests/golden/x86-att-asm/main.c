long asm_sum(const long *p, long n);
int asm_call(int x);
int helper(int x) { return x * 2; }
int main(void)
{
    static const long v[] = { 1, 2, 3, 4, 5 };
    if (asm_sum(v, 5) != 115 || asm_sum(v, 0) != 100) return 1;
    if (asm_call(20) != 41) return 2;
    return 42;
}
