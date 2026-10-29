/* __clear_cache(begin, end): libgcc's name for making the instructions
 * stored in [begin, end) the ones the processor fetches -- what
 * __builtin___clear_cache calls (src/ir/irgen.c), after a loader or a
 * JIT writes code. Where the caches are coherent or there are none to
 * speak of (x86-64, AVR, RX, ColdFire), the builtin emits nothing and
 * this does nothing; elsewhere, each architecture's own sequence:
 *
 *   AArch64     dc cvau and ic ivau over each line, by CTR_EL0's line
 *               sizes (skipped where CTR_EL0's IDC and DIC say no clean
 *               or invalidate is needed), dsb ish, isb
 *   ARMv7-A     the same through CP15: DCCMVAU, ICIMVAU, BPIALL, by CTR's
 *   Cortex-M    dsb, isb: an M-profile core has no caches architecture
 *               code can reach this way (a Cortex-M7's are the SCB's,
 *               which CMSIS maintains), as libgcc's does nothing
 *   RISC-V      fence.i (this hart)
 *   MIPS        synci over each line, by rdhwr $1's step (0: no caches),
 *               sync, and a return by jr.hb, which clears the hazards
 *   LoongArch   ibar 0
 *   PowerPC     dcbst over 32-byte lines, sync, icbi, sync, isync
 *   SPARC       flush every eight bytes
 *   TriCore     dsync, isync; Xtensa memw, isync
 *
 * On macOS and with glibc the system's __clear_cache comes first. */
void __clear_cache(void *begin, void *end);

#if defined(__mips__)
/* (all of it asm: the return is jr.hb) */
__attribute__((naked)) void __clear_cache(void *begin, void *end)
{
#if defined(__mips64)
#define CC_ADD "daddu"
#define CC_SUB "dsubu"
#else
#define CC_ADD "addu"
#define CC_SUB "subu"
#endif
    __asm__(".set noreorder\n"
            "rdhwr $v0, $1\n"            /* the synci step: 0, no caches */
            "beqz $v0, 2f\n"
            "nop\n"
            CC_SUB " $v1, $zero, $v0\n"
            "and $a0, $a0, $v1\n"        /* from begin's line */
            "1: synci 0($a0)\n"
            CC_ADD " $a0, $a0, $v0\n"
            "sltu $v1, $a0, $a1\n"
            "bnez $v1, 1b\n"
            "nop\n"
            "2: sync\n"
            "jr.hb $ra\n"
            "nop\n"
            ".set reorder\n");
}
#else
void __clear_cache(void *begin, void *end)
{
    unsigned long b = (unsigned long)begin, e = (unsigned long)end, a;
#if defined(__aarch64__)
    unsigned long ctr, dline, iline;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
    dline = 4UL << ((ctr >> 16) & 15);
    iline = 4UL << (ctr & 15);
    if (!(ctr >> 28 & 1))                                   /* IDC */
        for (a = b & ~(dline - 1); a < e; a += dline)
            __asm__ volatile ("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
    if (!(ctr >> 29 & 1))                                   /* DIC */
        for (a = b & ~(iline - 1); a < e; a += iline)
            __asm__ volatile ("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
#elif (defined(__arm__) || defined(__thumb__)) && defined(__ARM_ARCH_PROFILE) && \
      __ARM_ARCH_PROFILE == 'A'
    unsigned long ctr, dline, iline;
    __asm__ volatile ("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
    dline = 4UL << ((ctr >> 16) & 15);
    iline = 4UL << (ctr & 15);
    for (a = b & ~(dline - 1); a < e; a += dline)
        __asm__ volatile ("mcr p15, 0, %0, c7, c11, 1" :: "r"(a) : "memory");
    __asm__ volatile ("dsb" ::: "memory");
    for (a = b & ~(iline - 1); a < e; a += iline)
        __asm__ volatile ("mcr p15, 0, %0, c7, c5, 1" :: "r"(a) : "memory");
    __asm__ volatile ("mcr p15, 0, %0, c7, c5, 6" :: "r"(0UL) : "memory");
    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
#elif defined(__arm__) || defined(__thumb__)
    (void)b; (void)e; (void)a;
    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
#elif defined(__riscv)
    (void)b; (void)e; (void)a;
    __asm__ volatile ("fence.i" ::: "memory");
#elif defined(__loongarch__)
    (void)b; (void)e; (void)a;
    __asm__ volatile ("ibar 0" ::: "memory");
#elif defined(__PPC__) || defined(__powerpc__)
    for (a = b & ~31UL; a < e; a += 32)
        __asm__ volatile ("dcbst 0, %0" :: "r"(a) : "memory");
    __asm__ volatile ("sync" ::: "memory");
    for (a = b & ~31UL; a < e; a += 32)
        __asm__ volatile ("icbi 0, %0" :: "r"(a) : "memory");
    __asm__ volatile ("sync" ::: "memory");
    __asm__ volatile ("isync" ::: "memory");
#elif defined(__sparc__)
    for (a = b & ~7UL; a < e; a += 8)
        __asm__ volatile ("flush %0" :: "r"(a) : "memory");
#elif defined(__tricore__) || defined(__TRICORE__)
    (void)b; (void)e; (void)a;
    __asm__ volatile ("dsync" ::: "memory");
    __asm__ volatile ("isync" ::: "memory");
#elif defined(__XTENSA__) || defined(__xtensa__)
    (void)b; (void)e; (void)a;
    __asm__ volatile ("memw" ::: "memory");
    __asm__ volatile ("isync" ::: "memory");
#else
    (void)b; (void)e; (void)a;
#endif
}
#endif
