/* Instruction edges the exec corpus never reaches, run on QEMU and on
 * EmbSim (tests/golden/embsim.sh): each line prints what one instruction
 * computed, and the two must agree. Built by clang, whose assembler
 * takes every form here; EmbCC's inline assembler has no long multiply
 * and little VFP. A Cortex-M4F image for the mps2-an386, output and exit
 * through semihosting. */
extern unsigned __data_load, __data_start, __data_end, __bss_start, __bss_end;
void reset(void);
void fault(void);
void svc_handler(void);

__attribute__((section(".vectors"), used))
void (*const vectors[16])(void) = {
    (void (*)(void))0x20010000u, reset, fault, fault, fault, fault, fault,
    0, 0, 0, 0, svc_handler, fault, 0, fault, fault,
};

static void semi(unsigned op, const void *arg)
{
    __asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tbkpt #0xab"
                     : : "r"(op), "r"(arg) : "r0", "r1", "memory");
}

static void say(const char *s) { semi(4, s); }

static void sayx(unsigned v)
{
    char b[12];
    b[0] = ' ';
    for (int i = 0; i < 8; i++)
        b[1 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[9] = 0;
    say(b);
}

void fault(void)
{
    say("unexpected exception\n");
    for (;;)
        ;
}

static volatile unsigned svcs;
void svc_handler(void) { svcs++; }

static unsigned f2u(float f) { union { float f; unsigned u; } x = { f }; return x.u; }

int main(void)
{
    unsigned a, b, lo, hi;

    /* asrs sets C from the last bit shifted out; adc reads it */
    a = 0x80000004u;
    __asm__ volatile("asrs %0, %0, #3\n\tmov %1, #0\n\tadc %1, %1, #0"
                     : "+l"(a), "=r"(b) : : "cc");
    say("asr"); sayx(a); sayx(b);
    a = 0x80000004u;
    __asm__ volatile("asrs %0, %0, #2\n\tmov %1, #0\n\tadc %1, %1, #0"
                     : "+l"(a), "=r"(b) : : "cc");
    sayx(a); sayx(b); say("\n");

    /* sdiv: INT_MIN / -1 is INT_MIN, by the architecture */
    a = 0x80000000u; b = 0xffffffffu;
    __asm__ volatile("sdiv %0, %0, %1" : "+r"(a) : "r"(b));
    say("sdiv"); sayx(a); say("\n");

    /* umlal accumulates the 64-bit product */
    lo = 0xfffffff0u; hi = 1;
    a = 0x10000u; b = 0x30000u;
    __asm__ volatile("umlal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b));
    say("umlal"); sayx(hi); sayx(lo); say("\n");

    /* vcvt to an integer truncates; vcvtr would round */
    float x = 2.7f, y = -2.7f, z = 2.5f;
    int ix, iy, iz;
    __asm__ volatile("vcvt.s32.f32 %0, %1" : "=t"(x) : "t"(x));
    __asm__ volatile("vcvt.s32.f32 %0, %1" : "=t"(y) : "t"(y));
    __asm__ volatile("vcvt.s32.f32 %0, %1" : "=t"(z) : "t"(z));
    ix = (int)f2u(x); iy = (int)f2u(y); iz = (int)f2u(z);
    say("vcvt"); sayx((unsigned)ix); sayx((unsigned)iy); sayx((unsigned)iz); say("\n");

    /* vfms: d = d - n * m, fused */
    float d = 10.0f, n = 1.5f, m = 3.0f;
    __asm__ volatile("vfms.f32 %0, %1, %2" : "+t"(d) : "t"(n), "t"(m));
    float e = 1.0f;
    __asm__ volatile("vfma.f32 %0, %1, %2" : "+t"(e) : "t"(n), "t"(m));
    say("vfm"); sayx(f2u(d)); sayx(f2u(e)); say("\n");

    /* an exception taken with sp 4 below an 8-byte boundary: the frame
     * is aligned below it, xPSR bit 9 says so, and the return puts sp
     * back where it was. Taken at sp - 4 and at sp - 8, one of which is
     * 4 off a boundary whatever sp was. */
    unsigned before, after, before2, after2;
    __asm__ volatile("mov %0, sp\n\t"
                     "sub sp, sp, #4\n\t"
                     "svc #0\n\t"
                     "add sp, sp, #4\n\t"
                     "mov %1, sp"
                     : "=r"(before), "=r"(after) : : "memory");
    __asm__ volatile("mov %0, sp\n\t"
                     "sub sp, sp, #8\n\t"
                     "svc #0\n\t"
                     "add sp, sp, #8\n\t"
                     "mov %1, sp"
                     : "=r"(before2), "=r"(after2) : : "memory");
    say("stkalign"); sayx(after - before); sayx(after2 - before2); sayx(svcs);
    say("\n");
    return 0;
}

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    *(volatile unsigned *)0xE000ED88u |= (3u << 20) | (3u << 22);
    __asm__ volatile("dsb\n\tisb");
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; d++)
        *d = 0;
    static volatile unsigned blk[2];
    blk[0] = 0x20026u;
    blk[1] = (unsigned)main();
    semi(0x20, (const void *)blk);
    for (;;)
        ;
}
