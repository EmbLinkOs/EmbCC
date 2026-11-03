// asm goto in C++, as in C: the front end hands the statement on to the C
// it lowers to, labels and all -- here from a member function and a
// template, the label blocks reached only from the asm. Plain C++ where
// EmbCC's inline assembler has no branch for the template (x86-64).
// expect-exit: 42
struct Sign {
    int of(int x) const
    {
#if defined(__aarch64__)
        __asm__ goto("cbz %w0, %l[zero]\n\tcmp %w0, #0\n\tb.lt %l[neg]"
                     :: "r"(x) : "cc" : zero, neg);
#else
        if (x == 0) goto zero;
        if (x < 0) goto neg;
#endif
        return 1;
    zero:
        return 0;
    neg:
        return -1;
    }
};

template <class T> int is_small(T v)
{
#if defined(__aarch64__)
    __asm__ goto("cmp %0, #10\n\tb.lo %l1" :: "r"(v) : "cc" : yes);
#else
    if (v < 10) goto yes;
#endif
    return 0;
yes:
    return 1;
}

int main()
{
    Sign s;
    int t = s.of(5) + 2 * s.of(0) + 4 * s.of(-3);           // 1 + 0 - 4
    int u = is_small<unsigned long>(3) + is_small<unsigned long>(30);
    return t == -3 && u == 1 ? 42 : 1;
}
