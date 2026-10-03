// expect-exit: 42
/* __builtin_alloca_with_align meets its alignment. The argument was
 * dropped, and the memory came back only as aligned as the stack is. */
typedef unsigned long uptr;
__attribute__((noinline)) static int probe(int n)
{
    char *a = __builtin_alloca_with_align(n, 512);      /* 64 bytes */
    char *b = __builtin_alloca_with_align(n + 3, 2048); /* 256 bytes */
    char *c = __builtin_alloca_with_align(5, 64);       /* 8: the stack's */
    for (int i = 0; i < n; i++) a[i] = (char)i;
    for (int i = 0; i < n + 3; i++) b[i] = (char)(i + 1);
    c[0] = 7;
    if ((uptr)a % 64 || (uptr)b % 256 || (uptr)c % 8) return 1;
    for (int i = 0; i < n; i++) if (a[i] != (char)i) return 2;
    return c[0] == 7 ? 0 : 3;
}
int main(void)
{
    for (int n = 1; n < 40; n += 7)
        if (probe(n)) return probe(n);
    return 42;
}
