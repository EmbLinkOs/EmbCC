/* EmbSim's register file, over regs.svd on the mps2-an386: every access
 * type, modifiedWriteValues and readAction, reserved bits, narrow
 * registers and narrow accesses, a cluster array, a register array,
 * derivedFrom on a register and on a peripheral, and the bus faults where
 * no register is. Each line is a register's value after an access, and
 * regs.txt is what the CMSIS-SVD specification says it is.
 *
 * Output and the exit are semihosting's; the fault handler steps over the
 * faulting 16-bit load or store. No division: nothing from the runtime
 * library is needed. */
#define REG(a) (*(volatile unsigned *)(a))
#define REG8(a) (*(volatile unsigned char *)(a))
#define REG16(a) (*(volatile unsigned short *)(a))
#define SHCSR REG(0xE000ED24u)
#define CFSR REG(0xE000ED28u)
#define BFAR REG(0xE000ED38u)

#define ACC 0x40010000u
#define MWV 0x40011000u
#define ARR 0x40012000u
#define ARR2 0x40013000u

void reset(void);
void fault(void);
void bus_fault(void);

__attribute__((section(".vectors"), used))
void (*const vectors[16])(void) = {
    (void (*)(void))0x20004000u, reset, fault, fault, fault, bus_fault,
    fault, 0, 0, 0, 0, fault, fault, 0, fault, fault,
};

static void semi(unsigned op, const void *arg)
{
    __asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tbkpt #0xab"
                     : : "r"(op), "r"(arg) : "r0", "r1", "memory");
}

static void say(const char *s) { semi(4, s); }

static void leave(unsigned status)
{
    static volatile unsigned blk[2];
    blk[0] = 0x20026u;
    blk[1] = status;
    semi(0x20, (const void *)blk);
    for (;;)
        ;
}

static void show(const char *name, unsigned v)
{
    char b[10];
    for (int i = 0; i < 8; i++)
        b[i] = "0123456789abcdef"[v >> (28 - 4 * i) & 15];
    b[8] = '\n';
    b[9] = 0;
    say(name);
    say(" ");
    say(b);
}

void fault(void)
{
    say("unexpected exception\n");
    leave(1);
}

static volatile unsigned faults, fault_addr, fault_status;

void bus_fault_c(void)
{
    fault_status = CFSR;
    fault_addr = BFAR;
    CFSR = fault_status;
    faults++;
}

/* past the faulting instruction (a 16-bit ldr or str), then the C */
__attribute__((naked)) void bus_fault(void)
{
    __asm__ volatile("mrs r0, msp\n\t"
                     "ldr r1, [r0, #24]\n\t"
                     "adds r1, #2\n\t"
                     "str r1, [r0, #24]\n\t"
                     "b bus_fault_c");
}

static unsigned load(unsigned a)
{
    register unsigned r1 __asm__("r1") = a;
    register unsigned r0 __asm__("r0") = 0;
    __asm__ volatile("ldr r0, [r1]" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static void store(unsigned a, unsigned v)
{
    register unsigned r1 __asm__("r1") = a;
    register unsigned r0 __asm__("r0") = v;
    __asm__ volatile("str r0, [r1]" : : "r"(r0), "r"(r1) : "memory");
}

int main(void)
{
    SHCSR |= 1u << 17;                  /* BusFault on, not HardFault */

    /* access types */
    show("ACC.RW", REG(ACC + 0x00));
    REG(ACC + 0x00) = 0xCAFEF00Du;
    show("ACC.RW=cafef00d", REG(ACC + 0x00));
    REG8(ACC + 0x01) = 0x99;
    show("ACC.RW.byte1=99", REG(ACC + 0x00));
    REG16(ACC + 0x02) = 0x4321;
    show("ACC.RW.half1=4321", REG(ACC + 0x00));
    show("ACC.RO", REG(ACC + 0x04));
    REG(ACC + 0x04) = 0;
    show("ACC.RO=0", REG(ACC + 0x04));
    show("ACC.WO", REG(ACC + 0x08));
    REG(ACC + 0x08) = 0x55;
    show("ACC.WO=55", REG(ACC + 0x08));
    REG(ACC + 0x0C) = 1;
    show("ACC.ONCE=1", REG(ACC + 0x0C));
    show("ACC.RONCE", REG(ACC + 0x10));
    REG(ACC + 0x10) = 0x22;
    show("ACC.RONCE=22", REG(ACC + 0x10));
    REG(ACC + 0x10) = 0x33;
    show("ACC.RONCE=33", REG(ACC + 0x10));
    show("ACC.MIX", REG(ACC + 0x14));
    REG(ACC + 0x14) = 0;
    show("ACC.MIX=0", REG(ACC + 0x14));
    REG(ACC + 0x14) = 0x12345678u;
    show("ACC.MIX=12345678", REG(ACC + 0x14));
    /* B8, B8B and H16 are one word to a word access */
    show("ACC.B8", REG8(ACC + 0x18));
    show("ACC.H16", REG16(ACC + 0x1A));
    show("ACC.18", REG(ACC + 0x18));
    REG8(ACC + 0x18) = 0x11;
    REG16(ACC + 0x1A) = 0x1234;
    show("ACC.18.narrow", REG(ACC + 0x18));
    REG(ACC + 0x18) = 0xAABBCCDDu;
    show("ACC.18=aabbccdd", REG(ACC + 0x18));
    show("ACC.B8B", REG8(ACC + 0x19));
    show("ACC.LAST", REG(ACC + 0x20));

    /* modifiedWriteValues: one write, each nibble a different one */
    show("MWV.FLAGS", REG(MWV + 0x0));
    REG(MWV + 0x0) = 0x33333333u;
    show("MWV.FLAGS=33333333", REG(MWV + 0x0));
    REG8(MWV + 0x0) = 0;
    show("MWV.FLAGS.byte0=0", REG(MWV + 0x0));
    REG8(MWV + 0x1) = 0;
    show("MWV.FLAGS.byte1=0", REG(MWV + 0x0));
    REG(MWV + 0x4) = 0x0F;
    show("MWV.W1C=0f", REG(MWV + 0x4));
    /* readAction */
    show("MWV.EV", REG(MWV + 0x8));
    show("MWV.EV", REG(MWV + 0x8));
    show("MWV.RC", REG(MWV + 0xC));
    show("MWV.RC", REG(MWV + 0xC));
    REG(MWV + 0x10) = 2;
    show("MWV.MODE=2", REG(MWV + 0x10));

    /* clusters, arrays, derivedFrom */
    show("ARR.CH[0].CTRL", REG(ARR + 0x00));
    show("ARR.CH[2].DATA", REG(ARR + 0x24));
    REG(ARR + 0x18) = 0xFF;
    show("ARR.CH[1].STAT=ff", REG(ARR + 0x18));
    show("ARR.RC", REG(ARR + 0x48));
    REG(ARR + 0x4C) = 1;
    show("ARR.RD=1", REG(ARR + 0x4C));
    REG(ARR + 0x50) = 0x5610;
    show("ARR.BASE=5610", REG(ARR + 0x50));
    show("ARR.DER", REG(ARR + 0x54));
    REG(ARR + 0x54) = 0x5610;
    show("ARR.DER=5610", REG(ARR + 0x54));
    REG(ARR + 0x58) = 0xFF;
    show("ARR.FA=ff", REG(ARR + 0x58));
    REG(ARR + 0x00) = 0xBEEF;
    show("ARR.CH[0].CTRL=beef", REG(ARR + 0x00));
    show("ARR2.CH[0].CTRL", REG(ARR2 + 0x00));
    show("ARR2.DER", REG(ARR2 + 0x54));

    /* where no register is: a hole in a peripheral, the space between
     * two, a store; and past the SVD's last peripheral, the board's own */
    unsigned v = load(ACC + 0x1C);
    show("hole.read", v);
    show("hole.faults", faults);
    show("hole.BFAR", fault_addr);
    show("hole.CFSR", fault_status);
    store(ACC + 0x200, 1);
    show("between.faults", faults);
    show("between.BFAR", fault_addr);
    show("past.read", load(0x40020000u));
    show("past.faults", faults);
    say("done\n");
    return 0;
}

extern unsigned __data_load, __data_start, __data_end, __bss_start, __bss_end;

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    leave((unsigned)main());
}
