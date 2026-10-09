/* rv-isa.c -- RISC-V instruction edges for tests/golden/embsim-riscv.sh:
 * the cases whose results are easy to get wrong, each printed, so that
 * EmbSim's output can be held to QEMU's (and to the record).
 *
 * Built by clang for rv32gc/ilp32d and rv64gc/lp64d (EmbCC's inline
 * assembler has neither the rounding-mode operands nor the c. forms),
 * linked by embld at virt's RAM, and run there with no runtime: _start
 * turns the FPU on and calls main; the result ends the run through the
 * test device.
 *
 * The floating-point sweeps run every operation over a table of values
 * (zeros, ones, denormals, the extremes, infinities, quiet and
 * signalling NaNs, values that round) in every rounding mode, and print
 * one FNV-1a hash per operation and mode of the 64-bit register written
 * (so a single's NaN-boxing is in it) and of fflags. Built with
 * -DVERBOSE they print every result instead, to find the one that
 * differs. */
#include <stdint.h>

typedef unsigned long ulong;            /* XLEN */
#define XLEN __riscv_xlen

#define UART (*(volatile unsigned char *)0x10000000u)
#define TEST (*(volatile unsigned *)0x100000u)

static void putc_(int c) { UART = (unsigned char)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void hex(uint64_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        putc_("0123456789abcdef"[(v >> (4 * i)) & 15]);
}
static void line(const char *name, uint64_t v)
{
    puts_(name);
    putc_(' ');
    hex(v, 16);
    putc_('\n');
}

/* ---- traps -------------------------------------------------------------- */

static volatile ulong t_cause, t_epc, t_tval, t_count;

/* records the trap and returns past the instruction that took it */
__attribute__((interrupt("machine"), aligned(4))) static void handler(void)
{
    ulong c, e, v;
    __asm__ volatile("csrr %0, mcause\n\tcsrr %1, mepc\n\tcsrr %2, mtval"
                     : "=r"(c), "=r"(e), "=r"(v));
    t_cause = c;
    t_epc = e;
    t_tval = v;
    t_count++;
    unsigned short h = *(volatile unsigned short *)e;
    e += (h & 3) == 3 ? 4 : 2;
    __asm__ volatile("csrw mepc, %0" : : "r"(e));
}

static void trap_line(const char *name, ulong base)
{
    puts_(name);
    puts_(" cause ");
    hex(t_cause, XLEN / 4);
    puts_(" epc+");
    hex(t_epc - base, 4);
    puts_(" tval ");
    hex(t_tval, XLEN / 4);
    putc_('\n');
    t_cause = t_epc = t_tval = 0;
}

/* ---- M ------------------------------------------------------------------ */

#define RR(op) static ulong op##_(ulong a, ulong b) \
    { ulong r; __asm__ volatile(#op " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }
RR(mul) RR(mulh) RR(mulhsu) RR(mulhu) RR(div) RR(divu) RR(rem) RR(remu)
RR(slt) RR(sltu) RR(sra) RR(srl) RR(sll)
#if XLEN == 64
RR(mulw) RR(divw) RR(divuw) RR(remw) RR(remuw) RR(sraw) RR(srlw) RR(sllw)
RR(addw) RR(subw)
#endif

static const ulong ivals[] = {
    0, 1, (ulong)-1, 2, (ulong)-2, 7, (ulong)-7,
    (ulong)1 << (XLEN - 1), ((ulong)1 << (XLEN - 1)) - 1,
    0x80000000ul, 0x7fffffff, 0xffffffffu, 0x12345678,
#if XLEN == 64
    0x8000000000000001ul, 0x00000000fffffffful, 0xffffffff00000000ul,
#endif
};
#define NI (sizeof ivals / sizeof ivals[0])

static uint64_t fnv(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 0x100000001b3ull;
    }
    return h;
}

static void int_ops(void)
{
    static const struct { const char *name; ulong (*fn)(ulong, ulong); } ops[] = {
        { "mul", mul_ }, { "mulh", mulh_ }, { "mulhsu", mulhsu_ },
        { "mulhu", mulhu_ }, { "div", div_ }, { "divu", divu_ },
        { "rem", rem_ }, { "remu", remu_ }, { "slt", slt_ }, { "sltu", sltu_ },
        { "sra", sra_ }, { "srl", srl_ }, { "sll", sll_ },
#if XLEN == 64
        { "mulw", mulw_ }, { "divw", divw_ }, { "divuw", divuw_ },
        { "remw", remw_ }, { "remuw", remuw_ }, { "sraw", sraw_ },
        { "srlw", srlw_ }, { "sllw", sllw_ }, { "addw", addw_ },
        { "subw", subw_ },
#endif
    };
    for (unsigned k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned i = 0; i < NI; i++)
            for (unsigned j = 0; j < NI; j++) {
                ulong r = ops[k].fn(ivals[i], ivals[j]);
#ifdef VERBOSE
                puts_(ops[k].name); putc_(' ');
                hex(ivals[i], XLEN / 4); putc_(' ');
                hex(ivals[j], XLEN / 4); putc_(' ');
                hex(r, XLEN / 4); putc_('\n');
#endif
                h = fnv(h, r);
            }
        line(ops[k].name, h);
    }
    /* the ones a reader checks by eye */
    line("div-by-0", div_(7, 0));
    line("divu-by-0", divu_(7, 0));
    line("rem-by-0", rem_((ulong)-7, 0));
    line("remu-by-0", remu_(7, 0));
    line("div-overflow", div_((ulong)1 << (XLEN - 1), (ulong)-1));
    line("rem-overflow", rem_((ulong)1 << (XLEN - 1), (ulong)-1));
    line("mulhsu-neg", mulhsu_((ulong)-1, (ulong)-1));
}

/* ---- the compressed forms ----------------------------------------------- */

static void compressed(void)
{
    ulong r, s;
    __asm__ volatile(".option push\n.option rvc\n"
                     "mv %1, sp\n"
                     "c.addi16sp sp, -512\n"
                     "sub %0, %1, sp\n"
                     "c.addi16sp sp, 496\n"
                     "c.addi16sp sp, 16\n"
                     ".option pop" : "=&r"(r), "=&r"(s));
    line("c.addi16sp", r);
    __asm__ volatile(".option push\n.option rvc\n"
                     "c.addi4spn a0, sp, 1020\n"
                     "sub %0, a0, sp\n"
                     ".option pop" : "=r"(r) : : "a0");
    line("c.addi4spn", r);
    __asm__ volatile(".option push\n.option rvc\n"
                     "c.lui a0, 0xfffe1\n"
                     "mv %0, a0\n"
                     ".option pop" : "=r"(r) : : "a0");
    line("c.lui-neg", r);
    __asm__ volatile(".option push\n.option rvc\n"
                     "c.li a0, -32\n"
                     "c.srai a0, 1\n"
                     "c.andi a0, -17\n"
                     "c.slli a0, %1\n"
                     "c.srli a0, 3\n"
                     "mv %0, a0\n"
                     ".option pop" : "=r"(r) : "i"(XLEN - 8) : "a0");
    line("c.li-srai-andi-slli-srli", r);
    __asm__ volatile(".option push\n.option rvc\n"
                     "li a0, 0x5a5\n"
                     "li a1, 0x0ff\n"
                     "c.sub a0, a1\n"
                     "c.xor a0, a1\n"
                     "c.or a0, a1\n"
                     "c.and a0, a1\n"
                     "c.mv a1, a0\n"
                     "c.add a0, a1\n"
                     "mv %0, a0\n"
                     ".option pop" : "=r"(r) : : "a0", "a1");
    line("c.alu", r);
#if XLEN == 64
    __asm__ volatile(".option push\n.option rvc\n"
                     "li a0, 0x7fffffff\n"
                     "c.addiw a0, 1\n"
                     "li a1, 0x7fffffff\n"
                     "c.addw a1, a0\n"
                     "c.subw a0, a1\n"
                     "slli a0, a0, 32\n"
                     "or %0, a0, a1\n"
                     ".option pop" : "=r"(r) : : "a0", "a1");
    line("c.addiw-addw-subw", r);
#endif
    /* stack-relative loads and stores, at their largest offsets */
    static ulong area[64];
    ulong *base = area;
    __asm__ volatile(".option push\n.option rvc\n"
                     "mv t0, sp\n"
                     "mv sp, %1\n"
                     "li a0, -3\n"
                     "c.swsp a0, 252(sp)\n"
                     "c.lwsp a1, 252(sp)\n"
#if XLEN == 64
                     "c.sdsp a1, 504(sp)\n"
                     "c.ldsp a0, 504(sp)\n"
#else
                     "mv a0, a1\n"
#endif
                     "mv sp, t0\n"
                     "mv %0, a0\n"
                     ".option pop" : "=r"(r) : "r"(base) : "t0", "a0", "a1", "memory");
    line("c.swsp-lwsp-sdsp-ldsp", r);
    __asm__ volatile(".option push\n.option rvc\n"
                     "mv a2, %1\n"
                     "li a0, 0x1234\n"
                     "c.sw a0, 124(a2)\n"
                     "c.lw a1, 124(a2)\n"
#if XLEN == 64
                     "c.sd a1, 248(a2)\n"
                     "c.ld a0, 248(a2)\n"
#endif
                     "mv %0, a0\n"
                     ".option pop" : "=r"(r) : "r"(base) : "a0", "a1", "a2", "memory");
    line("c.sw-lw-sd-ld", r);
    double dv = -2.5;
    __asm__ volatile(".option push\n.option rvc\n"
                     "mv a2, %1\n"
                     "fld fa0, 0(%2)\n"
                     "c.fsd fa0, 248(a2)\n"
                     "c.fld fa1, 248(a2)\n"
                     "mv t0, sp\n"
                     "mv sp, a2\n"
                     "c.fsdsp fa1, 0(sp)\n"
                     "c.fldsp fa2, 0(sp)\n"
                     "mv sp, t0\n"
                     "fmv.x.w %0, fa2\n"
                     ".option pop" : "=r"(r) : "r"(base), "r"(&dv)
                     : "a2", "t0", "fa0", "fa1", "fa2", "memory");
    line("c.fsd-fld-fsdsp-fldsp", r);
#if XLEN == 32
    float fv = 1.75f;
    __asm__ volatile(".option push\n.option rvc\n"
                     "mv a2, %1\n"
                     "flw fa0, 0(%2)\n"
                     "c.fsw fa0, 124(a2)\n"
                     "c.flw fa1, 124(a2)\n"
                     "mv t0, sp\n"
                     "mv sp, a2\n"
                     "c.fswsp fa1, 252(sp)\n"
                     "c.flwsp fa2, 252(sp)\n"
                     "mv sp, t0\n"
                     "fmv.x.w %0, fa2\n"
                     ".option pop" : "=r"(r) : "r"(base), "r"(&fv)
                     : "a2", "t0", "fa0", "fa1", "fa2", "memory");
    line("c.fsw-flw-fswsp-flwsp", r);
    /* c.jal links two bytes on */
    __asm__ volatile(".option push\n.option rvc\n"
                     "mv t0, ra\n"
                     "c.jal 1f\n"
                     "1: auipc a0, 0\n"
                     "sub %0, a0, ra\n"
                     "mv ra, t0\n"
                     ".option pop" : "=r"(r) : : "t0", "a0");
    line("c.jal-link", r);
#endif
    /* c.jalr links two bytes on; c.jr; c.beqz, c.bnez; c.j */
    __asm__ volatile(".option push\n.option rvc\n"
                     "mv t0, ra\n"
                     "lla a1, 1f\n"
                     "c.jalr a1\n"
                     "1: sub %0, a1, ra\n"
                     "lla a1, 2f\n"
                     "c.jr a1\n"
                     "c.li %0, 9\n"
                     "2: li a0, 0\n"
                     "c.beqz a0, 3f\n"
                     "c.li %0, 9\n"
                     "3: c.bnez a0, 4f\n"
                     "c.j 5f\n"
                     "4: c.li %0, 9\n"
                     "5: mv ra, t0\n"
                     ".option pop" : "=&r"(r) : : "t0", "a0", "a1");
    line("c.jalr-jr-beqz-bnez-j", r);
    (void)s;
}

/* ---- misaligned access, LR/SC and the AMOs -------------------------------- */

static void memory(void)
{
    static unsigned char buf[32] __attribute__((aligned(8)));
    for (int i = 0; i < 32; i++)
        buf[i] = (unsigned char)(0x11 * i + 3);
    ulong r;
    __asm__ volatile("lw %0, 1(%1)" : "=r"(r) : "r"(buf));
    line("lw+1", r);
    __asm__ volatile("lhu %0, 3(%1)" : "=r"(r) : "r"(buf));
    line("lhu+3", r);
    __asm__ volatile("lh %0, 7(%1)" : "=r"(r) : "r"(buf));
    line("lh+7", r);
#if XLEN == 64
    __asm__ volatile("ld %0, 5(%1)" : "=r"(r) : "r"(buf));
    line("ld+5", r);
    __asm__ volatile("lwu %0, 9(%1)" : "=r"(r) : "r"(buf));
    line("lwu+9", r);
    __asm__ volatile("sd %1, 11(%0)" : : "r"(buf), "r"((ulong)0x0102030405060708ul) : "memory");
#endif
    __asm__ volatile("sw %1, 2(%0)\n\tsh %1, 21(%0)" : : "r"(buf), "r"((ulong)0xa1b2c3d4u) : "memory");
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 32; i++)
        h = fnv(h, buf[i]);
    line("misaligned-stores", h);

    static volatile ulong word __attribute__((aligned(8)));
    ulong old, sc;
    word = 5;
    __asm__ volatile("lr.w %0, (%2)\n\tsc.w %1, %3, (%2)"
                     : "=&r"(old), "=&r"(sc) : "r"(&word), "r"((ulong)9) : "memory");
    line("lr-sc", old << 8 | sc << 4 | (word & 15));
    __asm__ volatile("sc.w %0, %2, (%1)" : "=r"(sc) : "r"(&word), "r"((ulong)3) : "memory");
    line("sc-no-reservation", sc << 4 | (word & 15));
    __asm__ volatile("lr.w %0, (%2)\n\tsw %3, 0(%2)\n\tsc.w %1, %4, (%2)"
                     : "=&r"(old), "=&r"(sc) : "r"(&word), "r"((ulong)4), "r"((ulong)6) : "memory");
    line("sc-after-store", sc << 4 | (word & 15));
    __asm__ volatile("lr.w %0, (%2)\n\tsw %0, 0(%2)\n\tsc.w %1, %3, (%2)"
                     : "=&r"(old), "=&r"(sc) : "r"(&word), "r"((ulong)7) : "memory");
    line("sc-after-same-store", sc << 4 | (word & 15));

#define AMO(op, init, src) do { word = (ulong)(init); \
    __asm__ volatile(#op " %0, %2, (%1)" : "=r"(old) : "r"(&word), "r"((ulong)(src)) : "memory"); \
    line(#op, (uint64_t)old ^ ((uint64_t)word << 1)); } while (0)
    AMO(amoswap.w, 5, -1);
    AMO(amoadd.w, 0x7fffffff, 1);
    AMO(amoxor.w, 0xf0f0, 0xff00);
    AMO(amoand.w, 0xf0f0, 0xff00);
    AMO(amoor.w, 0xf0f0, 0xff00);
    AMO(amomin.w, 0x80000000u, 1);
    AMO(amomax.w, 0x80000000u, 1);
    AMO(amominu.w, 0x80000000u, 1);
    AMO(amomaxu.w, 0x80000000u, 1);
#if XLEN == 64
    AMO(amoswap.d, 5, -1);
    AMO(amoadd.d, 0x7fffffff, 1);
    AMO(amomin.d, 0x8000000000000000ul, 1);
    AMO(amomaxu.d, 0x8000000000000000ul, 1);
    AMO(amomin.w, 0x80000000u, 1);
#endif
}

/* ---- traps and the CSRs ----------------------------------------------------- */

static void traps(void)
{
    ulong base, r;
    __asm__ volatile("lla %0, 1f\n1: ecall" : "=r"(base));
    trap_line("ecall", base);
    __asm__ volatile("lla %0, 1f\n1: ebreak" : "=r"(base));
    trap_line("ebreak", base);
    __asm__ volatile("lla %0, 1f\n1: .option push\n.option rvc\nc.ebreak\n.option pop" : "=r"(base));
    trap_line("c.ebreak", base);
    __asm__ volatile("lla %0, 1f\n1: .word 0xffffffff" : "=r"(base));
    trap_line("illegal", base);
    __asm__ volatile("lla %0, 1f\n1: .half 0" : "=r"(base));
    trap_line("illegal-c", base);
    __asm__ volatile("lla %0, 1f\n1: csrw cycle, zero" : "=r"(base));
    trap_line("csr-read-only", base);
    __asm__ volatile("lla %0, 1f\n1: csrr a0, 0x7c0" : "=r"(base) : : "a0");
    trap_line("csr-none", base);
    __asm__ volatile("lla %0, 1f\n1: lw a0, 0(%1)" : "=&r"(base) : "r"(0x18000000ul) : "a0");
    trap_line("load-fault", base);
    __asm__ volatile("lla %0, 1f\n1: sw a0, 3(%1)" : "=&r"(base) : "r"(0x18000000ul) : "a0");
    trap_line("store-fault", base);
    static volatile unsigned w4[2];
    __asm__ volatile("lla %0, 1f\n1: amoadd.w a0, a0, (%1)" : "=&r"(base) : "r"((ulong)w4 + 2) : "a0", "memory");
    trap_line("amo-misaligned", base);
    __asm__ volatile("lla %0, 1f\n1: lr.w a0, (%1)" : "=&r"(base) : "r"((ulong)w4 + 1) : "a0", "memory");
    trap_line("lr-misaligned", base);
#if XLEN == 64
    __asm__ volatile("lla %0, 1f\n1: ld a0, 0(%1)" : "=&r"(base) : "r"(0x100000000ul) : "a0");
    trap_line("load-above-4g", base);
#endif
    /* mstatus after a trap: MPP machine, MPIE the MIE before it */
    __asm__ volatile("csrs mstatus, 8\n\tecall\n\tcsrr %0, mstatus\n\tcsrc mstatus, 8" : "=r"(r));
    line("mstatus-after-trap", (r >> 3 & 1) | (r >> 7 & 1) << 1 | (r >> 11 & 3) << 2);
    __asm__ volatile("csrr %0, mstatus" : "=r"(r));
    line("mstatus-after-mret", (r >> 3 & 1) | (r >> 7 & 1) << 1 | (r >> 11 & 3) << 2);
    t_cause = t_epc = t_tval = 0;

    /* the CSR instructions' read and write */
    __asm__ volatile("csrw mscratch, %1\n\tcsrrs %0, mscratch, %2" : "=&r"(r) : "r"((ulong)0x1200), "r"((ulong)0x34));
    line("csrrs-old", r);
    __asm__ volatile("csrrci %0, mscratch, 4\n\tcsrr %0, mscratch" : "=&r"(r));
    line("csrrci-new", r);
    __asm__ volatile("csrrwi %0, mscratch, 31\n\tcsrr %0, mscratch" : "=&r"(r));
    line("csrrwi-new", r);
    /* mtvec: a reserved mode is not written */
    ulong save;
    __asm__ volatile("csrr %1, mtvec\n\tcsrw mtvec, %2\n\tcsrr %0, mtvec\n\tcsrw mtvec, %1"
                     : "=&r"(r), "=&r"(save) : "r"((ulong)0x80000102u));
    line("mtvec-reserved-mode", r == save);
    __asm__ volatile("csrr %0, mepc" : "=r"(save));
    __asm__ volatile("csrw mepc, %1\n\tcsrr %0, mepc" : "=r"(r) : "r"((ulong)0x80000003u));
    line("mepc-low-bit", r);
    __asm__ volatile("csrw mepc, %0" : : "r"(save));
    /* the counters run forwards */
    ulong c0, c1, i0, i1;
    __asm__ volatile("csrr %0, mcycle\n\tcsrr %1, minstret\n\tnop\n\tnop\n\t"
                     "csrr %2, mcycle\n\tcsrr %3, minstret"
                     : "=&r"(c0), "=&r"(i0), "=&r"(c1), "=&r"(i1));
    line("counters-forward", (c1 > c0) | (i1 > i0) << 1);
    __asm__ volatile("rdcycle %0\n\trdinstret %1" : "=r"(c0), "=r"(i0));
    line("rd-counters-nonzero", (c0 != 0) | (i0 != 0) << 1);
    __asm__ volatile("csrr %0, mhartid" : "=r"(r));
    line("mhartid", r);
}

/* ---- F and D ---------------------------------------------------------------- */

static const uint32_t svals[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x40400000, 0x3eaaaaab,
    0x7f7fffff, 0xff7fffff, 0x00800000, 0x00000001, 0x807fffff, 0x7f800000,
    0xff800000, 0x7fc00000, 0x7fa00000, 0xffc00001, 0x4b000001, 0x3f000000,
    0x3fc00000, 0xc0200000, 0x4f000000, 0xcf000000, 0x5f800000, 0x34000000,
    0x0c000000, 0x33800000, 0x00ffffff, 0x3f7fffff, 0x4effffff, 0xdf000000,
};
static const uint64_t dvals[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull,
    0xbff0000000000000ull, 0x4008000000000000ull, 0x3fd5555555555555ull,
    0x7fefffffffffffffull, 0xffefffffffffffffull, 0x0010000000000000ull,
    0x0000000000000001ull, 0x800fffffffffffffull, 0x7ff0000000000000ull,
    0xfff0000000000000ull, 0x7ff8000000000000ull, 0x7ff4000000000000ull,
    0xfff8000000000001ull, 0x4330000000000001ull, 0x3fe0000000000000ull,
    0x3ff8000000000000ull, 0xc004000000000000ull, 0x41e0000000000000ull,
    0xc1e0000000000000ull, 0x43f0000000000000ull, 0x3cb0000000000000ull,
    0x0180000000000000ull, 0x3ca0000000000000ull, 0x001fffffffffffffull,
    0x3fefffffffffffffull, 0x41dfffffffffffffull, 0x47efffffe0000000ull,
    0x36a0000000000000ull, 0x3ff0000000000001ull,
};
#define NS (sizeof svals / sizeof svals[0])
#define ND (sizeof dvals / sizeof dvals[0])

/* the operands in memory; a single as a NaN-boxed double */
static uint64_t opa, opb, opc, res;
static ulong ires;
static ulong flg;

#define FLDS "fld ft0, %[a]\n\tfld ft1, %[b]\n\tfld ft2, %[c]\n\t"
#define IO [a] "m"(opa), [b] "m"(opb), [c] "m"(opc)

/* an operation in one rounding mode: R the result register, F the form */
#define FOP(name, insn, rm) static void name(void) { \
    __asm__ volatile("csrw fflags, zero\n\t" FLDS insn " ft3, ft0, ft1" rm "\n\t" \
                     "fsd ft3, %[r]\n\tfrflags %[f]" \
                     : [r] "=m"(res), [f] "=r"(flg) : IO : "ft0", "ft1", "ft2", "ft3"); }
#define FOP3(name, insn, rm) static void name(void) { \
    __asm__ volatile("csrw fflags, zero\n\t" FLDS insn " ft3, ft0, ft1, ft2" rm "\n\t" \
                     "fsd ft3, %[r]\n\tfrflags %[f]" \
                     : [r] "=m"(res), [f] "=r"(flg) : IO : "ft0", "ft1", "ft2", "ft3"); }
#define FOP1(name, insn, rm) static void name(void) { \
    __asm__ volatile("csrw fflags, zero\n\t" FLDS insn " ft3, ft0" rm "\n\t" \
                     "fsd ft3, %[r]\n\tfrflags %[f]" \
                     : [r] "=m"(res), [f] "=r"(flg) : IO : "ft0", "ft1", "ft2", "ft3"); }
#define FTOI(name, insn, rm) static void name(void) { \
    __asm__ volatile("csrw fflags, zero\n\t" FLDS insn " %[i], ft0" rm "\n\t" \
                     "frflags %[f]" \
                     : [i] "=r"(ires), [f] "=r"(flg) : IO : "ft0", "ft1", "ft2"); }
#define ITOF(name, insn, rm) static void name(void) { \
    __asm__ volatile("csrw fflags, zero\n\t" insn " ft3, %[x]" rm "\n\t" \
                     "fsd ft3, %[r]\n\tfrflags %[f]" \
                     : [r] "=m"(res), [f] "=r"(flg) : [x] "r"((ulong)opa) : "ft3"); }
#define FCMP(name, insn) static void name(void) { \
    __asm__ volatile("csrw fflags, zero\n\t" FLDS insn " %[i], ft0, ft1\n\t" \
                     "frflags %[f]" \
                     : [i] "=r"(ires), [f] "=r"(flg) : IO : "ft0", "ft1", "ft2"); }

#define RM5(M, name, insn) M(name##_rne, insn, ", rne") M(name##_rtz, insn, ", rtz") \
    M(name##_rdn, insn, ", rdn") M(name##_rup, insn, ", rup") M(name##_rmm, insn, ", rmm")
#define FP_OPS(M3, M2, M1, s) \
    RM5(M2, fadd_##s, "fadd." #s) RM5(M2, fsub_##s, "fsub." #s) \
    RM5(M2, fmul_##s, "fmul." #s) RM5(M2, fdiv_##s, "fdiv." #s) \
    RM5(M1, fsqrt_##s, "fsqrt." #s) RM5(M3, fmadd_##s, "fmadd." #s) \
    RM5(M3, fmsub_##s, "fmsub." #s) RM5(M3, fnmadd_##s, "fnmadd." #s) \
    RM5(M3, fnmsub_##s, "fnmsub." #s)
FP_OPS(FOP3, FOP, FOP1, s)
FP_OPS(FOP3, FOP, FOP1, d)
FOP(fadd_s_dyn, "fadd.s", "")
FOP(fmul_d_dyn, "fmul.d", "")
FOP(fsgnj_s, "fsgnj.s", "") FOP(fsgnjn_s, "fsgnjn.s", "") FOP(fsgnjx_s, "fsgnjx.s", "")
FOP(fsgnj_d, "fsgnj.d", "") FOP(fsgnjn_d, "fsgnjn.d", "") FOP(fsgnjx_d, "fsgnjx.d", "")
FOP(fmin_s, "fmin.s", "") FOP(fmax_s, "fmax.s", "")
FOP(fmin_d, "fmin.d", "") FOP(fmax_d, "fmax.d", "")
FCMP(feq_s, "feq.s") FCMP(flt_s, "flt.s") FCMP(fle_s, "fle.s")
FCMP(feq_d, "feq.d") FCMP(flt_d, "flt.d") FCMP(fle_d, "fle.d")
FTOI(fclass_s_, "fclass.s", "") FTOI(fclass_d_, "fclass.d", "")
RM5(FTOI, fcvt_w_s, "fcvt.w.s") RM5(FTOI, fcvt_wu_s, "fcvt.wu.s")
RM5(FTOI, fcvt_w_d, "fcvt.w.d") RM5(FTOI, fcvt_wu_d, "fcvt.wu.d")
RM5(FOP1, fcvt_s_d, "fcvt.s.d") RM5(FOP1, fcvt_d_s, "fcvt.d.s")
RM5(ITOF, fcvt_s_w, "fcvt.s.w") RM5(ITOF, fcvt_s_wu, "fcvt.s.wu")
RM5(ITOF, fcvt_d_w, "fcvt.d.w") RM5(ITOF, fcvt_d_wu, "fcvt.d.wu")
#if XLEN == 64
RM5(FTOI, fcvt_l_s, "fcvt.l.s") RM5(FTOI, fcvt_lu_s, "fcvt.lu.s")
RM5(FTOI, fcvt_l_d, "fcvt.l.d") RM5(FTOI, fcvt_lu_d, "fcvt.lu.d")
RM5(ITOF, fcvt_s_l, "fcvt.s.l") RM5(ITOF, fcvt_s_lu, "fcvt.s.lu")
RM5(ITOF, fcvt_d_l, "fcvt.d.l") RM5(ITOF, fcvt_d_lu, "fcvt.d.lu")
#endif

/* how an operation is swept: its operands' format and count, and what it
 * writes */
enum { F2, F3, F1, TOI, CMP, FROMI };
struct fop {
    const char *name;
    void (*fn)(void);
    int d, kind;
};
#define R5(name, d, k) { #name ".rne", name##_rne, d, k }, { #name ".rtz", name##_rtz, d, k }, \
    { #name ".rdn", name##_rdn, d, k }, { #name ".rup", name##_rup, d, k }, \
    { #name ".rmm", name##_rmm, d, k }
static const struct fop fops[] = {
    R5(fadd_s, 0, F2), R5(fsub_s, 0, F2), R5(fmul_s, 0, F2), R5(fdiv_s, 0, F2),
    R5(fsqrt_s, 0, F1), R5(fmadd_s, 0, F3), R5(fmsub_s, 0, F3),
    R5(fnmadd_s, 0, F3), R5(fnmsub_s, 0, F3),
    R5(fadd_d, 1, F2), R5(fsub_d, 1, F2), R5(fmul_d, 1, F2), R5(fdiv_d, 1, F2),
    R5(fsqrt_d, 1, F1), R5(fmadd_d, 1, F3), R5(fmsub_d, 1, F3),
    R5(fnmadd_d, 1, F3), R5(fnmsub_d, 1, F3),
    { "fsgnj.s", fsgnj_s, 0, F2 }, { "fsgnjn.s", fsgnjn_s, 0, F2 },
    { "fsgnjx.s", fsgnjx_s, 0, F2 }, { "fsgnj.d", fsgnj_d, 1, F2 },
    { "fsgnjn.d", fsgnjn_d, 1, F2 }, { "fsgnjx.d", fsgnjx_d, 1, F2 },
    { "fmin.s", fmin_s, 0, F2 }, { "fmax.s", fmax_s, 0, F2 },
    { "fmin.d", fmin_d, 1, F2 }, { "fmax.d", fmax_d, 1, F2 },
    { "feq.s", feq_s, 0, CMP }, { "flt.s", flt_s, 0, CMP }, { "fle.s", fle_s, 0, CMP },
    { "feq.d", feq_d, 1, CMP }, { "flt.d", flt_d, 1, CMP }, { "fle.d", fle_d, 1, CMP },
    { "fclass.s", fclass_s_, 0, TOI }, { "fclass.d", fclass_d_, 1, TOI },
    R5(fcvt_w_s, 0, TOI), R5(fcvt_wu_s, 0, TOI), R5(fcvt_w_d, 1, TOI),
    R5(fcvt_wu_d, 1, TOI), R5(fcvt_s_d, 1, F1), R5(fcvt_d_s, 0, F1),
    R5(fcvt_s_w, 0, FROMI), R5(fcvt_s_wu, 0, FROMI), R5(fcvt_d_w, 1, FROMI),
    R5(fcvt_d_wu, 1, FROMI),
#if XLEN == 64
    R5(fcvt_l_s, 0, TOI), R5(fcvt_lu_s, 0, TOI), R5(fcvt_l_d, 1, TOI),
    R5(fcvt_lu_d, 1, TOI), R5(fcvt_s_l, 0, FROMI), R5(fcvt_s_lu, 0, FROMI),
    R5(fcvt_d_l, 1, FROMI), R5(fcvt_d_lu, 1, FROMI),
#endif
};

static const uint64_t ints[] = {
    0, 1, 0xffffffffffffffffull, 0x7fffffff, 0x80000000, 0xffffffff,
    0x7fffff81, 0x1000001, 0xfffffffffeffffffull, 0x20000000000001ull,
    0x8000000000000000ull, 0x7fffffffffffffffull, 0xfffffffffffff800ull,
    0x123456789abcdefull, 0xfffffffe00000001ull, 0x1fffffffffffffull,
};
#define NINT (sizeof ints / sizeof ints[0])

static uint64_t box(uint32_t v)
{
    return 0xffffffff00000000ull | v;
}

static uint64_t val(int d, unsigned i)
{
    return d ? dvals[i] : box(svals[i]);
}

static uint64_t h;

static void one(const struct fop *o)
{
    o->fn();
    uint64_t r = o->kind == TOI || o->kind == CMP ? (uint64_t)ires : res;
    h = fnv(fnv(h, r), flg);
#ifdef VERBOSE
    puts_(o->name); putc_(' ');
    hex(opa, 16); putc_(' '); hex(opb, 16); putc_(' '); hex(opc, 16);
    putc_(' '); hex(r, 16); putc_(' '); hex(flg, 2); putc_('\n');
#endif
}

static void fp_sweep(void)
{
    for (unsigned k = 0; k < sizeof fops / sizeof fops[0]; k++) {
        const struct fop *o = &fops[k];
        unsigned n = o->d ? ND : NS;
        h = 0xcbf29ce484222325ull;
        switch (o->kind) {
        case F1:
        case TOI:
            for (unsigned i = 0; i < n; i++) {
                opa = val(o->d, i);
                one(o);
            }
            break;
        case FROMI:
            for (unsigned i = 0; i < NINT; i++) {
                opa = ints[i];
                one(o);
            }
            break;
        case F3:
            /* a third of the table each, for size */
            for (unsigned i = 0; i < n; i += 2)
                for (unsigned j = 1; j < n; j += 2)
                    for (unsigned l = 0; l < n; l += 3) {
                        opa = val(o->d, i);
                        opb = val(o->d, j);
                        opc = val(o->d, l);
                        one(o);
                    }
            break;
        default:
            for (unsigned i = 0; i < n; i++)
                for (unsigned j = 0; j < n; j++) {
                    opa = val(o->d, i);
                    opb = val(o->d, j);
                    one(o);
                }
        }
        line(o->name, h);
    }
}

static void fp_edges(void)
{
    ulong r, base;
    /* a single not NaN-boxed reads as the canonical NaN */
    opa = 0x000000003f800000ull;
    opb = box(0x3f800000);
    fadd_s_rne();
    line("unboxed-add", res);
    fsgnjn_s();
    line("unboxed-fsgnjn", res);
    __asm__ volatile("fld ft0, %1\n\tfmv.x.w %0, ft0" : "=r"(r) : "m"(opa) : "ft0");
    line("unboxed-fmv.x.w", r);
    /* fmv.w.x boxes */
    __asm__ volatile("fmv.w.x ft0, %1\n\tfsd ft0, %0" : "=m"(res) : "r"((ulong)0x80000001u) : "ft0");
    line("fmv.w.x-boxes", res);
    /* the dynamic rounding mode, in each frm */
    for (ulong m = 0; m < 5; m++) {
        opa = box(0x3f800000);
        opb = box(0x33800001);
        __asm__ volatile("csrw frm, %0" : : "r"(m));
        fadd_s_dyn();
        line("fadd.s-dyn", res << 8 | flg);
        opa = dvals[5];
        opb = dvals[4];
        fmul_d_dyn();
        line("fmul.d-dyn", res ^ flg);
    }
    /* frm 5: a dynamic rounding mode is illegal */
    __asm__ volatile("csrwi frm, 5\n\tlla %0, 1f\n1: fadd.s ft0, ft0, ft0" : "=r"(base) : : "ft0");
    trap_line("frm-5-dyn", base);
    __asm__ volatile("csrwi frm, 0");
    /* fcsr: frm and fflags in one */
    __asm__ volatile("csrw fcsr, %1\n\tcsrr %0, fcsr" : "=r"(r) : "r"((ulong)0xffff));
    line("fcsr", r);
    __asm__ volatile("csrr %0, frm" : "=r"(r));
    line("frm", r);
    __asm__ volatile("csrrci %0, fflags, 0x15\n\tcsrr %0, fflags" : "=&r"(r));
    line("fflags-csrrci", r);
    __asm__ volatile("fsflags zero\n\tfsrm zero");
    /* mstatus.FS: dirty after an operation, and off makes F illegal */
    __asm__ volatile("csrc mstatus, %1\n\tcsrs mstatus, %2\n\tfmv.w.x ft0, zero\n\tcsrr %0, mstatus"
                     : "=r"(r) : "r"((ulong)0x6000), "r"((ulong)0x2000) : "ft0");
    line("fs-dirty", (r >> 13 & 3) | (r >> (XLEN - 1)) << 2);
    __asm__ volatile("csrc mstatus, %1\n\tlla %0, 1f\n1: fadd.d ft0, ft0, ft0\n\tcsrs mstatus, %2"
                     : "=&r"(base) : "r"((ulong)0x6000), "r"((ulong)0x2000) : "ft0");
    trap_line("fs-off", base);
}

/* ---- the run ----------------------------------------------------------------- */

int main(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(handler));
    int_ops();
    compressed();
    memory();
    traps();
    fp_edges();
    fp_sweep();
    puts_("traps taken: ");
    hex(t_count, 4);
    putc_('\n');
    return 0;
}

void _start(void)
{
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x2000ul));
    main();
    TEST = 0x5555;
    for (;;)
        ;
}
