/* GNU C's asm goto: a template that branches to C labels -- the one a
 * value picks, two from one asm, %l[name] and %lN (counted past the
 * outputs and inputs), an output on the fall-through path, the label
 * blocks kept at -O2 though only the asm reaches them, inside a loop. And
 * a label inside an ordinary template (a loop in asm). Plain C where
 * EmbCC's inline assembler has no branch for the template (x86-64). */
// expect-exit: 42
#if defined(__aarch64__)
#define CLASSIFY(x) __asm__ goto("cbz %w0, %l[zero]\n\tcmp %w0, #0\n\tb.lt %l2" \
                                 :: "r"(x) : "cc" : zero, neg)
#define TRIPLE(r, n) __asm__("mov %w0, #0\n1:\n\tadd %w0, %w0, #3\n\tsub %w1, %w1, #1\n\tcbnz %w1, 1b" \
                             : "=&r"(r), "+r"(n))
#elif defined(__thumb__) || defined(__arm__)
#define CLASSIFY(x) __asm__ goto("cmp %0, #0\n\tbeq %l[zero]\n\tblt %l2" \
                                 :: "r"(x) : "cc" : zero, neg)
#define TRIPLE(r, n) __asm__("movs %0, #0\n1:\n\tadds %0, %0, #3\n\tsubs %1, %1, #1\n\tbne 1b" \
                             : "=&r"(r), "+r"(n) :: "cc")
#elif defined(__riscv)
#define CLASSIFY(x) __asm__ goto("beqz %0, %l[zero]\n\tbltz %0, %l2" \
                                 :: "r"(x) :: zero, neg)
#define TRIPLE(r, n) __asm__("li %0, 0\n1:\n\taddi %0, %0, 3\n\taddi %1, %1, -1\n\tbnez %1, 1b" \
                             : "=&r"(r), "+r"(n))
#endif

static int classify(int x)
{
#ifdef CLASSIFY
    CLASSIFY(x);
#else
    if (x == 0)
        goto zero;
    if (x < 0)
        goto neg;
#endif
    return 1;
zero:
    return 0;
neg:
    return -1;
}

static int triple(int n)
{
    int r;
#ifdef TRIPLE
    TRIPLE(r, n);
#else
    r = 3 * n;
#endif
    return r;
}

int main(void)
{
    static volatile int in[] = { 5, 0, -7, 0, 9, -1 };
    int pos = 0, zer = 0, neg = 0;
    for (int i = 0; i < 6; i++) {
        int c = classify(in[i]);
        pos += c == 1;
        zer += c == 0;
        neg += c == -1;
    }
    if (pos != 2 || zer != 2 || neg != 2)
        return 1;
    if (triple(4) != 12 || triple(1) != 3)
        return 2;
    return 42;
}
