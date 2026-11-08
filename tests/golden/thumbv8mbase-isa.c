/* What ARMv8-M Baseline adds to ARMv6-M, each instruction once, for
 * thumbv8mbase-exec to run on QEMU's M33 and on EmbSim's Cortex-M23:
 * MOVW/MOVT, SDIV/UDIV (a divide by zero gives 0: Baseline has no trap),
 * CBZ/CBNZ, B.W, LDREX/STREX and the byte form, LDAEX/STLEX, LDA/STL,
 * CLREX. Built with -DMAINLINE it also runs ADD.W, which the M23 lacks:
 * there the run must fault. */
int main(void)
{
    unsigned x, v, st;
    static unsigned w;
    static unsigned char wb;
    __asm__ volatile("movw %0, #0x1234\n\tmovt %0, #0x5678" : "=r"(x));
    if (x != 0x56781234u) return 1;
    {
        volatile int a = -100, b = 7;
        int q;
        unsigned u;
        volatile unsigned z = 0;
        __asm__ volatile("sdiv %0, %1, %2" : "=r"(q) : "r"(a), "r"(b));
        if (q != -14) return 2;
        __asm__ volatile("udiv %0, %1, %2" : "=r"(u) : "r"(100u), "r"(z));
        if (u != 0) return 3;
    }
    __asm__ volatile("1: ldaex %0, [%2]\n\tadds %0, #5\n\tstlex %1, %0, [%2]\n\t"
                     "cmp %1, #0\n\tbne 1b" : "=&l"(v), "=&l"(st) : "l"(&w)
                     : "cc", "memory");
    __asm__ volatile("1: ldrex %0, [%2]\n\tadds %0, #1\n\tstrex %1, %0, [%2]\n\t"
                     "cmp %1, #0\n\tbne 1b" : "=&l"(v), "=&l"(st) : "l"(&w)
                     : "cc", "memory");
    if (w != 6) return 4;
    __asm__ volatile("1: ldrexb %0, [%2]\n\tadds %0, #3\n\tstrexb %1, %0, [%2]\n\t"
                     "cmp %1, #0\n\tbne 1b" : "=&l"(v), "=&l"(st) : "l"(&wb)
                     : "cc", "memory");
    if (wb != 3) return 5;
    __asm__ volatile("movs %0, #77\n\tstl %0, [%1]\n\tmovs %0, #0\n\tlda %0, [%1]"
                     : "=&l"(v) : "l"(&w) : "memory");
    if (v != 77 || w != 77) return 6;
    __asm__ volatile("ldrex %0, [%2]\n\tclrex\n\tstrex %1, %0, [%2]"
                     : "=&l"(v), "=&l"(st) : "l"(&w) : "memory");
    if (st != 1 || w != 77) return 7;         /* the monitor was cleared */
    __asm__ volatile("movs %0, #0\n\tcbz %0, 1f\n\tmovs %0, #9\n1:\n\t"
                     "cbnz %0, 2f\n\tb.w 3f\n2:\n\tmovs %0, #8\n3:" : "=&l"(v) :: "cc");
    if (v != 0) return 8;
#ifdef MAINLINE
    __asm__ volatile("add.w %0, %0, #1" : "+r"(v));
#endif
    return 42;
}
