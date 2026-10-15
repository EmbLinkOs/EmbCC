/* mips-isa.c -- MIPS32r2 instruction edges for tests/golden/embsim-mips.sh:
 * the cases whose results are easy to get wrong, each printed, so that
 * EmbSim's output can be held to QEMU's (malta, 24Kc) and to the record.
 *
 * Built by clang for mipsel (mips32r2, no PIC), linked by embld at
 * malta's RAM, and run with no runtime: _start (embld's -Tstack stub)
 * calls main, whose end resets the board through SOFTRES.
 *
 * Covered: lwl, lwr, swl and swr at each byte offset; ext, ins, seb,
 * seh, wsbh, the shifts, rotr and rotrv; clz and clo; the multiplies into HI and
 * LO, madd and msub (signed and unsigned), mul, div and divu (and their
 * cases the architecture leaves unpredictable, as QEMU computes them);
 * movz and movn; the set-on-less-thans with negative immediates; the
 * likely branches, which skip their delay slot when not taken; the
 * linking branches, which link when not taken; jalr's link; the
 * exceptions -- overflow, the traps, syscall, break, misaligned loads
 * and stores with BadVAddr, the FPU's coprocessor unusable (a 24Kc has
 * none), a reserved instruction (but not COP2: a core with no CP2 takes
 * Coprocessor Unusable there, as the architecture says and EmbSim does,
 * where QEMU raises Reserved Instruction), an exception in a delay slot (EPC the branch, Cause.BD);
 * ll and sc, and eret clearing the link; rdhwr; the TLB: an entry
 * written and used, tlbp, and the refill exception at its own vector;
 * the core timer's interrupt waking a wait. */
#include <stdint.h>

#define UART_THR (*(volatile unsigned char *)0xbf000900u)
#define UART_LSR (*(volatile unsigned char *)0xbf000928u)
#define SOFTRES (*(volatile unsigned *)0xbf000500u)

static void putc_(int c)
{
    while (!(UART_LSR & 0x20))
        ;
    UART_THR = (unsigned char)c;
}
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void hex(uint32_t v)
{
    for (int i = 7; i >= 0; i--)
        putc_("0123456789abcdef"[(v >> (4 * i)) & 15]);
}
static void line(const char *name, uint32_t v)
{
    puts_(name);
    putc_(' ');
    hex(v);
    putc_('\n');
}
static void line2(const char *name, uint32_t a, uint32_t b)
{
    puts_(name);
    putc_(' ');
    hex(a);
    putc_(' ');
    hex(b);
    putc_('\n');
}

/* ---- exceptions ------------------------------------------------------- */

/* cause, badvaddr, epc, resume (0: past the instruction), taken, the
 * refill vector's */
volatile uint32_t exc_info[6];

__asm__(
    "    .text\n"
    "    .set push\n"
    "    .set noreorder\n"
    "    .set noat\n"
    "    .globl exc_handler\n"
    "exc_handler:\n"
    "    lui   $27, %hi(exc_info)\n"
    "    addiu $27, $27, %lo(exc_info)\n"
    "    mfc0  $26, $13\n"
    "    sw    $26, 0($27)\n"
    "    andi  $26, $26, 0x7c\n"
    "    bnez  $26, 2f\n"
    "    nop\n"
    /* the timer's interrupt: Compare written again clears it */
    "    mfc0  $26, $11\n"
    "    mtc0  $26, $11\n"
    "    li    $26, 1\n"
    "    sw    $26, 16($27)\n"
    "    ehb\n"
    "    eret\n"
    "2:  mfc0  $26, $8\n"
    "    sw    $26, 4($27)\n"
    "    mfc0  $26, $14\n"
    "    sw    $26, 8($27)\n"
    "    lw    $26, 12($27)\n"
    "    bnez  $26, 1f\n"
    "    nop\n"
    "    mfc0  $26, $14\n"
    "    addiu $26, $26, 4\n"
    "1:  mtc0  $26, $14\n"
    "    sw    $0, 12($27)\n"
    "    li    $26, 1\n"
    "    sw    $26, 16($27)\n"
    "    ehb\n"
    "    eret\n"
    "    nop\n"
    "    .globl refill_handler\n"
    "refill_handler:\n"
    "    lui   $27, %hi(exc_info)\n"
    "    addiu $27, $27, %lo(exc_info)\n"
    "    li    $26, 1\n"
    "    sw    $26, 20($27)\n"
    "    j     exc_handler\n"
    "    nop\n"
    "    .set pop\n");
void exc_handler(void);
void refill_handler(void);

static void vector(uint32_t at, void (*to)(void))
{
    volatile uint32_t *v = (volatile uint32_t *)at;
    uint32_t h = (uint32_t)to;
    v[0] = 0x3c1a0000u | (h >> 16);                 /* lui  k0, %hi */
    v[1] = 0x375a0000u | (h & 0xffffu);             /* ori  k0, k0, %lo */
    v[2] = 0x03400008u;                             /* jr   k0 */
    v[3] = 0;                                       /* nop */
}

static void exc_line(const char *name)
{
    puts_(name);
    if (!exc_info[4]) {
        puts_(" none\n");
        return;
    }
    puts_(" code ");
    hex((exc_info[0] >> 2) & 31);
    puts_(" ce ");
    hex((exc_info[0] >> 28) & 3);
    puts_(" bd ");
    hex(exc_info[0] >> 31);
    putc_('\n');
    exc_info[4] = 0;
}

/* ---- the instructions --------------------------------------------------- */

#define RR(name, op) static uint32_t name(uint32_t a, uint32_t b) \
    { uint32_t r; __asm__ volatile(op " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }
RR(rotrv_, "rotrv") RR(mul_, "mul") RR(slt_, "slt") RR(sltu_, "sltu")

static void hilo(const char *name, uint32_t hi, uint32_t lo)
{
    line2(name, hi, lo);
}

static void test_lwlr(void)
{
    static volatile uint8_t buf[8] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7 };
    for (int k = 0; k < 4; k++) {
        uint32_t l = 0x11223344u, r = 0x11223344u;
        __asm__ volatile("lwl %0, 0(%1)" : "+r"(l) : "r"(buf + k));
        __asm__ volatile("lwr %0, 0(%1)" : "+r"(r) : "r"(buf + k));
        line2("lwl/lwr", l, r);
    }
    for (int k = 0; k < 4; k++) {
        volatile uint32_t m[2] = { 0, 0 };
        __asm__ volatile("swl %0, 0(%1)" : : "r"(0xc1c2c3c4u), "r"((volatile uint8_t *)m + k) : "memory");
        line2("swl", m[0], m[1]);
        m[0] = m[1] = 0;
        __asm__ volatile("swr %0, 0(%1)" : : "r"(0xc1c2c3c4u), "r"((volatile uint8_t *)m + k) : "memory");
        line2("swr", m[0], m[1]);
    }
    /* the unaligned word, as a compiler's ulw/usw pair writes it */
    uint32_t u = 0;
    __asm__ volatile("lwl %0, 4(%1)\n\tlwr %0, 1(%1)" : "+r"(u) : "r"(buf));
    line("ulw+1", u);
}

static void test_bits(void)
{
    uint32_t x = 0x80ff7f01u, r;
    __asm__ volatile("ext %0, %1, 4, 8" : "=r"(r) : "r"(x)); line("ext 4,8", r);
    __asm__ volatile("ext %0, %1, 0, 32" : "=r"(r) : "r"(x)); line("ext 0,32", r);
    __asm__ volatile("ext %0, %1, 31, 1" : "=r"(r) : "r"(x)); line("ext 31,1", r);
    r = 0x12345678u;
    __asm__ volatile("ins %0, %1, 8, 12" : "+r"(r) : "r"(x)); line("ins 8,12", r);
    r = 0x12345678u;
    __asm__ volatile("ins %0, %1, 0, 32" : "+r"(r) : "r"(x)); line("ins 0,32", r);
    r = 0x12345678u;
    __asm__ volatile("ins %0, %1, 31, 1" : "+r"(r) : "r"(x)); line("ins 31,1", r);
    __asm__ volatile("seb %0, %1" : "=r"(r) : "r"(x)); line("seb", r);
    __asm__ volatile("seb %0, %1" : "=r"(r) : "r"(0x7fu)); line("seb 7f", r);
    __asm__ volatile("seh %0, %1" : "=r"(r) : "r"(x)); line("seh", r);
    __asm__ volatile("seh %0, %1" : "=r"(r) : "r"(0x8000u)); line("seh 8000", r);
    __asm__ volatile("wsbh %0, %1" : "=r"(r) : "r"(x)); line("wsbh", r);
    __asm__ volatile("rotr %0, %1, 0" : "=r"(r) : "r"(x)); line("rotr 0", r);
    __asm__ volatile("rotr %0, %1, 1" : "=r"(r) : "r"(x)); line("rotr 1", r);
    __asm__ volatile("rotr %0, %1, 31" : "=r"(r) : "r"(x)); line("rotr 31", r);
    __asm__ volatile("sra %0, %1, 4" : "=r"(r) : "r"(x)); line("sra 4", r);
    __asm__ volatile("srl %0, %1, 4" : "=r"(r) : "r"(x)); line("srl 4", r);
    __asm__ volatile("srav %0, %1, %2" : "=r"(r) : "r"(x), "r"(35u)); line("srav 35", r);
    __asm__ volatile("srlv %0, %1, %2" : "=r"(r) : "r"(x), "r"(35u)); line("srlv 35", r);
    __asm__ volatile("sllv %0, %1, %2" : "=r"(r) : "r"(x), "r"(33u)); line("sllv 33", r);
    line("rotrv 36", rotrv_(36, x));
    line("rotrv 0", rotrv_(0, x));
    static const uint32_t cz[] = { 0, 1, 0x80000000u, 0xffffffffu, 0x00f00000u, 0xfff0ffffu };
    for (unsigned i = 0; i < sizeof cz / sizeof cz[0]; i++) {
        uint32_t z, o;
        __asm__ volatile("clz %0, %1" : "=r"(z) : "r"(cz[i]));
        __asm__ volatile("clo %0, %1" : "=r"(o) : "r"(cz[i]));
        line2("clz/clo", z, o);
    }
}

static void test_muldiv(void)
{
    static const uint32_t v[][2] = {
        { 7, 3 }, { 0xfffffff9u, 3 }, { 0x80000000u, 0xffffffffu },
        { 0xffffffffu, 0xffffffffu }, { 0x12345678u, 0 }, { 0, 0 },
        { 0x7fffffffu, 0x7fffffffu }, { 0x80000000u, 2 },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint32_t a = v[i][0], b = v[i][1], hi, lo;
        __asm__ volatile("mult %2, %3\n\tmfhi %0\n\tmflo %1" : "=r"(hi), "=r"(lo) : "r"(a), "r"(b));
        hilo("mult", hi, lo);
        __asm__ volatile("multu %2, %3\n\tmfhi %0\n\tmflo %1" : "=r"(hi), "=r"(lo) : "r"(a), "r"(b));
        hilo("multu", hi, lo);
        __asm__ volatile("div $0, %2, %3\n\tmfhi %0\n\tmflo %1" : "=r"(hi), "=r"(lo) : "r"(a), "r"(b));
        hilo("div", hi, lo);
        __asm__ volatile("divu $0, %2, %3\n\tmfhi %0\n\tmflo %1" : "=r"(hi), "=r"(lo) : "r"(a), "r"(b));
        hilo("divu", hi, lo);
        line("mul", mul_(a, b));
        __asm__ volatile("mthi %2\n\tmtlo %3\n\tmadd %4, %5\n\tmfhi %0\n\tmflo %1"
                         : "=r"(hi), "=r"(lo) : "r"(0x1u), "r"(0xfffffff0u), "r"(a), "r"(b));
        hilo("madd", hi, lo);
        __asm__ volatile("mthi %2\n\tmtlo %3\n\tmaddu %4, %5\n\tmfhi %0\n\tmflo %1"
                         : "=r"(hi), "=r"(lo) : "r"(0x1u), "r"(0xfffffff0u), "r"(a), "r"(b));
        hilo("maddu", hi, lo);
        __asm__ volatile("mthi %2\n\tmtlo %3\n\tmsub %4, %5\n\tmfhi %0\n\tmflo %1"
                         : "=r"(hi), "=r"(lo) : "r"(0x1u), "r"(0x10u), "r"(a), "r"(b));
        hilo("msub", hi, lo);
        __asm__ volatile("mthi %2\n\tmtlo %3\n\tmsubu %4, %5\n\tmfhi %0\n\tmflo %1"
                         : "=r"(hi), "=r"(lo) : "r"(0x1u), "r"(0x10u), "r"(a), "r"(b));
        hilo("msubu", hi, lo);
    }
}

static void test_misc(void)
{
    uint32_t r = 5;
    __asm__ volatile("movz %0, %1, %2" : "+r"(r) : "r"(9u), "r"(0u)); line("movz taken", r);
    __asm__ volatile("movz %0, %1, %2" : "+r"(r) : "r"(7u), "r"(1u)); line("movz not", r);
    __asm__ volatile("movn %0, %1, %2" : "+r"(r) : "r"(3u), "r"(2u)); line("movn taken", r);
    __asm__ volatile("movn %0, %1, %2" : "+r"(r) : "r"(4u), "r"(0u)); line("movn not", r);
    line("slt", slt_(0xffffffffu, 1));
    line("sltu", sltu_(0xffffffffu, 1));
    __asm__ volatile("sltiu %0, %1, -1" : "=r"(r) : "r"(0xfffffffeu)); line("sltiu -1", r);
    __asm__ volatile("slti %0, %1, -1" : "=r"(r) : "r"(0xfffffffeu)); line("slti -1", r);
    __asm__ volatile("sltiu %0, %1, -32768" : "=r"(r) : "r"(0xffff7fffu)); line("sltiu -8000", r);
}

/* the delay slot adds 1 to n; the branch's target adds 16 */
#define LIKELY(name, br, val) do { \
    uint32_t n = 0; \
    __asm__ volatile(".set push\n\t.set noreorder\n\t" br " %1, 1f\n\t" \
                     "addiu %0, %0, 1\n\taddiu %0, %0, 0x100\n1:\taddiu %0, %0, 16\n\t.set pop" \
                     : "+r"(n) : "r"(val)); \
    line(name, n); } while (0)
#define LIKELY2(name, br, a, b) do { \
    uint32_t n = 0; \
    __asm__ volatile(".set push\n\t.set noreorder\n\t" br " %1, %2, 1f\n\t" \
                     "addiu %0, %0, 1\n\taddiu %0, %0, 0x100\n1:\taddiu %0, %0, 16\n\t.set pop" \
                     : "+r"(n) : "r"(a), "r"(b)); \
    line(name, n); } while (0)

static void test_branches(void)
{
    LIKELY2("beql taken", "beql", 1u, 1u);
    LIKELY2("beql not", "beql", 1u, 2u);
    LIKELY2("bnel taken", "bnel", 1u, 2u);
    LIKELY2("bnel not", "bnel", 1u, 1u);
    LIKELY2("beq not", "beq", 1u, 2u);
    LIKELY("blezl taken", "blezl", 0u);
    LIKELY("blezl not", "blezl", 1u);
    LIKELY("bgtzl taken", "bgtzl", 1u);
    LIKELY("bgtzl not", "bgtzl", 0xffffffffu);
    LIKELY("bltzl taken", "bltzl", 0x80000000u);
    LIKELY("bltzl not", "bltzl", 0u);
    LIKELY("bgezl taken", "bgezl", 0u);
    LIKELY("bgezl not", "bgezl", 0xffffffffu);
    uint32_t ra, here;
    /* the link is written whether or not the branch is taken */
    __asm__ volatile(".set push\n\t.set noreorder\n\tmove $8, $31\n\t"
                     "bltzal %2, 1f\n\tnop\n1:\tmove %0, $31\n\tmove $31, $8\n\t"
                     "bal 2f\n\tnop\n2:\tmove %1, $31\n\tmove $31, $8\n\t.set pop"
                     : "=r"(ra), "=r"(here) : "r"(0u) : "$8");
    line("bltzal links", ra - here);
    __asm__ volatile(".set push\n\t.set noreorder\n\tmove $8, $31\n\t"
                     "bgezal %2, 1f\n\tnop\n\taddiu $9, $9, 1\n1:\tmove %0, $31\n\tmove $31, $8\n\t"
                     "bal 2f\n\tnop\n2:\tmove %1, $31\n\tmove $31, $8\n\t.set pop"
                     : "=r"(ra), "=r"(here) : "r"(0xffffffffu) : "$8", "$9");
    line("bgezal not taken links", ra - here);
    __asm__ volatile(".set push\n\t.set noreorder\n\tla $9, 1f\n\t"
                     "jalr %0, $9\n\tnop\n1:\tbal 2f\n\tnop\n2:\tmove %1, $31\n\t.set pop"
                     : "=r"(ra), "=r"(here) : : "$9", "$31");
    line("jalr link", here - ra);
}

#define EXC(name, insn, ...) do { \
    exc_info[4] = 0; \
    __asm__ volatile(".set push\n\t.set noreorder\n\t" insn "\n\t.set pop" : : __VA_ARGS__); \
    exc_line(name); } while (0)

static void test_exceptions(void)
{
    uint32_t big = 0x7fffffffu, neg = 0x80000000u;
    EXC("add ov", "add $8, %0, %0", "r"(big) : "$8");
    EXC("add ok", "add $8, %0, %1", "r"(big), "r"(neg) : "$8");
    EXC("addi ov", "addi $8, %0, 1", "r"(big) : "$8");
    EXC("addi ok", "addi $8, %0, -1", "r"(big) : "$8");
    EXC("sub ov", "sub $8, %0, %1", "r"(neg), "r"(1u) : "$8");
    EXC("sub ok", "sub $8, %0, %1", "r"(big), "r"(1u) : "$8");
    EXC("teq", "teq %0, %0", "r"(1u));
    EXC("tne", "tne %0, %0", "r"(1u));
    EXC("tge", "tge %0, %1", "r"(1u), "r"(0xffffffffu));
    EXC("tgeu", "tgeu %0, %1", "r"(1u), "r"(0xffffffffu));
    EXC("tlt", "tlt %0, %1", "r"(0xffffffffu), "r"(1u));
    EXC("tltu", "tltu %0, %1", "r"(0xffffffffu), "r"(1u));
    EXC("teqi", "teqi %0, -1", "r"(0xffffffffu));
    EXC("tnei", "tnei %0, -1", "r"(0xffffffffu));
    EXC("tgei", "tgei %0, -2", "r"(0xffffffffu));
    EXC("tgeiu", "tgeiu %0, -2", "r"(0xffffffffu));
    EXC("tlti", "tlti %0, 0", "r"(0xffffffffu));
    EXC("tltiu", "tltiu %0, 0", "r"(0xffffffffu));
    EXC("syscall", "syscall", "r"(0));
    EXC("break", "break 7", "r"(0));
    static volatile uint32_t w[2];
    uint32_t a = (uint32_t)w + 1;
    EXC("lw misaligned", "lw $8, 0(%0)", "r"(a) : "$8");
    line("badvaddr", exc_info[1] - (uint32_t)w);
    EXC("lh misaligned", "lh $8, 0(%0)", "r"(a) : "$8");
    EXC("lhu aligned", "lhu $8, 1(%0)", "r"(a) : "$8");
    EXC("sw misaligned", "sw $8, 2(%0)", "r"(a) : "$8");
    line("badvaddr", exc_info[1] - (uint32_t)w);
    EXC("sh misaligned", "sh $8, 0(%0)", "r"(a) : "$8");
    EXC("lwc1", ".word 0xc4000000", "r"(0));
    EXC("reserved", ".word 0xfc000000", "r"(0));
    EXC("kseg2 unmapped", "lw $8, 0(%0)", "r"(0xc0000000u) : "$8");
    /* an exception in a delay slot: EPC is the branch, Cause.BD set; the
     * handler resumes at 1: */
    exc_info[4] = 0;
    uint32_t br;
    __asm__ volatile(".set push\n\t.set noreorder\n\tla $9, 1f\n\tla %0, 2f\n\t"
                     "sw $9, 12(%1)\n2:\tbeq $0, $0, 1f\n\tlw $8, 0(%2)\n\t"
                     "nop\n1:\t.set pop" : "=&r"(br) : "r"(exc_info), "r"(a) : "$8", "$9", "memory");
    line("delay slot epc-branch", exc_info[2] - br);
    exc_line("delay slot");
}

static void test_llsc(void)
{
    static volatile uint32_t word = 5;
    uint32_t v, ok;
    __asm__ volatile("ll %0, 0(%2)\n\taddiu %0, %0, 1\n\tsc %0, 0(%2)\n\tmove %1, %0"
                     : "=&r"(v), "=r"(ok) : "r"(&word) : "memory");
    line2("ll/sc", ok, word);
    /* an eret between them clears the link */
    __asm__ volatile("ll %0, 0(%2)\n\taddiu %0, %0, 1\n\tsyscall\n\tsc %0, 0(%2)\n\tmove %1, %0"
                     : "=&r"(v), "=r"(ok) : "r"(&word) : "memory");
    line2("ll/eret/sc", ok, word);
    exc_info[4] = 0;
}

static void test_rdhwr(void)
{
    uint32_t r;
    __asm__ volatile(".set push\n\t.set mips32r2\n\trdhwr %0, $0\n\t.set pop" : "=r"(r));
    line("rdhwr cpunum", r);
    __asm__ volatile(".set push\n\t.set mips32r2\n\trdhwr %0, $3\n\t.set pop" : "=r"(r));
    line("rdhwr ccres", r);
    __asm__ volatile("sync\n\tsynci 0($sp)\n\tpref 0, 0($sp)");
    puts_("sync synci pref ok\n");
}

/* one TLB entry: kuseg 0x00400000 (an even 4 KiB page) on RAM at
 * physical 0x00200000, then an access to a page no entry maps */
static void test_tlb(void)
{
    volatile uint32_t *phys = (volatile uint32_t *)0x80200010u;
    *phys = 0xfeedf00du;
    __asm__ volatile("mtc0 $0, $5\n\t"                      /* PageMask: 4 KiB */
                     "mtc0 %0, $10\n\t"                     /* EntryHi: VPN2, ASID 0 */
                     "mtc0 %1, $2\n\t"                      /* EntryLo0: PFN, D, V, G */
                     "mtc0 %2, $3\n\t"                      /* EntryLo1: invalid, G */
                     "mtc0 %3, $0\n\t"                      /* Index */
                     "ehb\n\ttlbwi\n\tehb"
                     : : "r"(0x00400000u), "r"((0x200u << 6) | 7u), "r"(1u), "r"(5u));
    uint32_t v = *(volatile uint32_t *)0x00400010u;
    line("tlb mapped read", v);
    *(volatile uint32_t *)0x00400014u = 0x12345678u;
    line("tlb mapped write", phys[1]);
    uint32_t idx;
    __asm__ volatile("mtc0 %1, $10\n\tehb\n\ttlbp\n\tehb\n\tmfc0 %0, $0" : "=r"(idx) : "r"(0x00400000u));
    line("tlbp hit", idx);
    __asm__ volatile("mtc0 %1, $10\n\tehb\n\ttlbp\n\tehb\n\tmfc0 %0, $0" : "=r"(idx) : "r"(0x00800000u));
    line("tlbp miss", idx >> 31);
    exc_info[4] = exc_info[5] = 0;
    __asm__ volatile("lw $8, 0(%0)" : : "r"(0x00800004u) : "$8");
    exc_line("tlb refill");
    line2("refill vector, badvaddr", exc_info[5], exc_info[1]);
    uint32_t hi;
    __asm__ volatile("mfc0 %0, $10" : "=r"(hi));
    line("entryhi vpn2", hi & 0xffffe000u);
    /* the odd page of the pair is invalid: an exception at the general vector */
    exc_info[4] = exc_info[5] = 0;
    __asm__ volatile("lw $8, 0(%0)" : : "r"(0x00401000u) : "$8");
    exc_line("tlb invalid");
    line("refill vector", exc_info[5]);
    /* a store through a clean (D clear) page */
    __asm__ volatile("mtc0 %0, $10\n\tmtc0 %1, $2\n\tmtc0 %2, $3\n\tmtc0 %3, $0\n\t"
                     "ehb\n\ttlbwi\n\tehb"
                     : : "r"(0x00600000u), "r"((0x200u << 6) | 3u), "r"(1u), "r"(6u));
    exc_info[4] = 0;
    __asm__ volatile("sw $0, 0(%0)" : : "r"(0x00600000u));
    exc_line("tlb modified");
}

/* Count and Compare: the timer's interrupt on IP7 wakes a wait */
static void test_timer(void)
{
    uint32_t st, c;
    exc_info[4] = 0;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    __asm__ volatile("mtc0 %0, $11\n\tehb" : : "r"(c + 2000));
    __asm__ volatile("mfc0 %0, $12" : "=r"(st));
    __asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"((st & ~0xff00u) | 0x8001u));
    while (!exc_info[4])
        __asm__ volatile("wait");
    __asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(st));
    line("timer interrupt code", (exc_info[0] >> 2) & 31);
    line("timer ip7", (exc_info[0] >> 15) & 1);
    exc_info[4] = 0;
}

int main(void)
{
    vector(0x80000180u, exc_handler);
    vector(0x80000000u, refill_handler);
    uint32_t st;
    __asm__ volatile("mfc0 %0, $12" : "=r"(st));
    st &= ~((1u << 22) | (1u << 2) | (1u << 1) | 1u);    /* BEV, ERL, EXL, IE */
    __asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(st));
    test_lwlr();
    test_bits();
    test_muldiv();
    test_misc();
    test_branches();
    test_exceptions();
    test_llsc();
    test_rdhwr();
    test_tlb();
    test_timer();
    puts_("done\n");
    SOFTRES = 0x42;
    for (;;)
        ;
}

void _start(void)
{
    main();
}
