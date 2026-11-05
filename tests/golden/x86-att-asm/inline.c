static int popsum(unsigned long x)
{
    int n;
    __asm__("xorl %0, %0\n1:\n\ttestq %1, %1\n\tjz 2f\n\tleaq -1(%1), %%rcx\n\tandq %%rcx, %1\n\tincl %0\n\tjmp 1b\n2:"
            : "=&r"(n), "+r"(x) :: "rcx", "cc");
    return n;
}
static int cas(int *p, int old, int new)
{
    int prev;
    __asm__ volatile("lock cmpxchgl %2, %1" : "=a"(prev), "+m"(*p) : "r"(new), "0"(old) : "memory");
    return prev;
}
static int top_bit(unsigned v) { int r; __asm__("bsrl %1, %0" : "=r"(r) : "rm"(v)); return r; }
static unsigned char lowbyte(unsigned v) { unsigned char r; __asm__("movb %b1, %0" : "=r"(r) : "r"(v)); return r; }
static double dadd(double a, double b) { __asm__("addsd %1, %0" : "+x"(a) : "x"(b)); return a; }
static int sign(long x)
{
    __asm__ goto("testq %0, %0\n\tjz %l[zero]\n\tjs %l2" :: "r"(x) : "cc" : zero, neg);
    return 1;
zero: return 0;
neg: return -1;
}
int main(void)
{
    int v = 5;
    if (popsum(0xf0f0) != 8 || popsum(0) != 0) return 1;
    if (cas(&v, 5, 9) != 5 || v != 9 || cas(&v, 5, 1) != 9 || v != 9) return 2;
    if (top_bit(0x80) != 7 || top_bit(1) != 0) return 3;
    if (lowbyte(0x1234) != 0x34) return 4;
    if (dadd(1.5, 2.25) != 3.75) return 5;
    if (sign(5) != 1 || sign(0) != 0 || sign(-3) != -1) return 6;
    return 42;
}
