/* mips-fpu.c -- MIPS32r2 FPU edges for tests/golden/embsim-mips.sh, run
 * on QEMU's malta with a P5600 (FR 1, the 2008 NaN and abs modes) and
 * on EmbSim's malta with the pic32mz core, whose FPU is a PIC32MZ EF's.
 * The results are printed, so EmbSim's output can be held to QEMU's (and
 * to the record).
 *
 * Built by clang for mipsel (mips32r2, -mfp64, hard float), linked by
 * embld at malta's RAM; _start (embld's -Tstack stub) calls main, which
 * turns the FPU on (Status.CU1 and FR) and resets the board when done.
 *
 * Each operation runs over a table of values (zeros, ones, values that
 * round, the extremes, a subnormal, infinities, a quiet and a signalling
 * NaN) in each of FCSR's four
 * rounding modes, and one FNV-1a hash per operation and mode is printed
 * of the 64-bit result and of FCSR afterwards (its Cause and Flags). A
 * NaN result is hashed as one value: QEMU's 2008 mode propagates an
 * operand's payload, EmbSim's arithmetic gives the default NaN, and the
 * architecture allows either. Also: the sixteen compare conditions into
 * an FCC bit and bc1t, movf and movt, mfhc1/mthc1 and the 64-bit
 * registers, ldc1/sdc1 and the indexed forms, the conversions' invalid
 * cases, and an enabled division by zero and invalid operation taking
 * the floating-point exception. Built with -DVERBOSE it prints every result instead. */
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

static uint64_t h;
static void mix(uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 0x100000001b3ull;
    }
}

static int isnan_d(uint64_t v)
{
    return ((v >> 52) & 0x7ff) == 0x7ff && (v & 0xfffffffffffffull);
}
static int isnan_s(uint32_t v)
{
    return ((v >> 23) & 0xff) == 0xff && (v & 0x7fffff);
}

static void record(const char *name, uint64_t r, uint32_t fcsr, int d)
{
#ifdef VERBOSE
    puts_(name);
    putc_(' ');
    hex(r, 16);
    putc_(' ');
    hex(fcsr, 8);
    putc_('\n');
#else
    (void)name;
#endif
    if (d ? isnan_d(r) : isnan_s((uint32_t)r))
        r = 0x7ff8dead7ff8deadull;
    mix(r);
    mix(fcsr & 0x0003f07fu);                    /* Cause, Flags, RM */
}

static const uint64_t dv[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull,
    0xbff0000000000000ull, 0x3ff8000000000000ull, 0x4008000000000000ull,
    0x3fb999999999999aull, 0x7fefffffffffffffull, 0x0010000000000000ull,
    0x0000000000000001ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x400921fb54442d18ull, 0xc1e0000000000000ull, 0x41dfffffffc00000ull,
    0x3ff0000000000001ull, 0x000fffffffffffffull, 0x3fefffffffffffffull,
    0x7ff8000000000000ull, 0x7ff0000000000001ull,
};
#define ND (int)(sizeof dv / sizeof dv[0])
static const uint32_t sv[] = {
    0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x3fc00000u,
    0x40400000u, 0x3dcccccdu, 0x7f7fffffu, 0x00800000u, 0x00000001u,
    0x7f800000u, 0xff800000u, 0x40490fdbu, 0x3f7fffffu, 0x7fc00000u,
    0x7f800001u,
};
#define NS (int)(sizeof sv / sizeof sv[0])

/* FCSR set to rm with no flags, an operation on $f0 and $f2 into $f4 */
#define SET_RM(rm) __asm__ volatile("ctc1 %0, $31" : : "r"(rm))
static uint32_t get_fcsr(void)
{
    uint32_t v;
    __asm__ volatile("cfc1 %0, $31" : "=r"(v));
    return v;
}

#define LOAD2_D(a, b) __asm__ volatile("mtc1 %0, $f0\n\tmthc1 %1, $f0\n\tmtc1 %2, $f2\n\tmthc1 %3, $f2" \
        : : "r"((uint32_t)(a)), "r"((uint32_t)((a) >> 32)), "r"((uint32_t)(b)), "r"((uint32_t)((b) >> 32)))
#define LOAD3_D(a, b, c) __asm__ volatile("mtc1 %0, $f6\n\tmthc1 %1, $f6" \
        : : "r"((uint32_t)(c)), "r"((uint32_t)((c) >> 32))); LOAD2_D(a, b)
#define LOAD2_S(a, b) __asm__ volatile("mtc1 %0, $f0\n\tmtc1 %1, $f2" : : "r"(a), "r"(b))
#define RES_D(v) do { uint32_t lo_, hi_; \
        __asm__ volatile("mfc1 %0, $f4\n\tmfhc1 %1, $f4" : "=r"(lo_), "=r"(hi_)); \
        v = (uint64_t)hi_ << 32 | lo_; } while (0)
#define RES_S(v) do { uint32_t lo_; __asm__ volatile("mfc1 %0, $f4" : "=r"(lo_)); v = lo_; } while (0)

#define BIN_D(nm, insn) static void nm(void) { \
    for (uint32_t rm = 0; rm < 4; rm++) { h = 0xcbf29ce484222325ull; \
        for (int i = 0; i < ND; i++) for (int j = 0; j < ND; j++) { \
            uint64_t r; SET_RM(rm); LOAD2_D(dv[i], dv[j]); \
            __asm__ volatile(insn " $f4, $f0, $f2" ::: "memory"); \
            RES_D(r); record(#nm, r, get_fcsr(), 1); } \
        puts_(#nm " rm"); putc_('0' + (int)rm); putc_(' '); hex(h, 16); putc_('\n'); } }
#define UN_D(nm, insn, outd) static void nm(void) { \
    for (uint32_t rm = 0; rm < 4; rm++) { h = 0xcbf29ce484222325ull; \
        for (int i = 0; i < ND; i++) { \
            uint64_t r; SET_RM(rm); LOAD2_D(dv[i], dv[0]); \
            __asm__ volatile(insn " $f4, $f0" ::: "memory"); \
            if (outd) RES_D(r); else RES_S(r); record(#nm, r, get_fcsr(), outd); } \
        puts_(#nm " rm"); putc_('0' + (int)rm); putc_(' '); hex(h, 16); putc_('\n'); } }
#define BIN_S(nm, insn) static void nm(void) { \
    for (uint32_t rm = 0; rm < 4; rm++) { h = 0xcbf29ce484222325ull; \
        for (int i = 0; i < NS; i++) for (int j = 0; j < NS; j++) { \
            uint64_t r; SET_RM(rm); LOAD2_S(sv[i], sv[j]); \
            __asm__ volatile(insn " $f4, $f0, $f2" ::: "memory"); \
            RES_S(r); record(#nm, r, get_fcsr(), 0); } \
        puts_(#nm " rm"); putc_('0' + (int)rm); putc_(' '); hex(h, 16); putc_('\n'); } }
#define TER_D(nm, insn) static void nm(void) { \
    for (uint32_t rm = 0; rm < 4; rm++) { h = 0xcbf29ce484222325ull; \
        for (int i = 0; i < ND; i += 2) for (int j = 1; j < ND; j += 2) for (int k = 0; k < ND; k += 3) { \
            uint64_t r; SET_RM(rm); LOAD3_D(dv[i], dv[j], dv[k]); \
            __asm__ volatile(insn " $f4, $f6, $f0, $f2" ::: "memory"); \
            RES_D(r); record(#nm, r, get_fcsr(), 1); } \
        puts_(#nm " rm"); putc_('0' + (int)rm); putc_(' '); hex(h, 16); putc_('\n'); } }

BIN_D(add_d, "add.d") BIN_D(sub_d, "sub.d") BIN_D(mul_d, "mul.d") BIN_D(div_d, "div.d")
BIN_S(add_s, "add.s") BIN_S(mul_s, "mul.s") BIN_S(div_s, "div.s")
UN_D(sqrt_d, "sqrt.d", 1) UN_D(recip_d, "recip.d", 1) UN_D(rsqrt_d, "rsqrt.d", 1)
UN_D(cvt_s_d, "cvt.s.d", 0) UN_D(cvt_w_d, "cvt.w.d", 0) UN_D(cvt_l_d, "cvt.l.d", 1)
UN_D(round_w_d, "round.w.d", 0) UN_D(trunc_w_d, "trunc.w.d", 0)
UN_D(ceil_w_d, "ceil.w.d", 0) UN_D(floor_w_d, "floor.w.d", 0)
UN_D(abs_d, "abs.d", 1) UN_D(neg_d, "neg.d", 1)
TER_D(madd_d, "madd.d") TER_D(msub_d, "msub.d") TER_D(nmadd_d, "nmadd.d") TER_D(nmsub_d, "nmsub.d")

static void conversions_from(void)
{
    static const uint32_t w[] = { 0, 1, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x12345679u };
    static const uint64_t l[] = { 0, 1, ~0ull, 0x7fffffffffffffffull, 0x8000000000000000ull,
                                  0x0123456789abcdefull };
    for (uint32_t rm = 0; rm < 4; rm++) {
        h = 0xcbf29ce484222325ull;
        for (int i = 0; i < 6; i++) {
            uint64_t r;
            SET_RM(rm);
            __asm__ volatile("mtc1 %0, $f0\n\tcvt.d.w $f4, $f0" : : "r"(w[i]) : "memory");
            RES_D(r); record("cvt.d.w", r, get_fcsr(), 1);
            SET_RM(rm);
            __asm__ volatile("mtc1 %0, $f0\n\tcvt.s.w $f4, $f0" : : "r"(w[i]) : "memory");
            RES_S(r); record("cvt.s.w", r, get_fcsr(), 0);
            SET_RM(rm);
            __asm__ volatile("mtc1 %0, $f0\n\tmthc1 %1, $f0\n\tcvt.d.l $f4, $f0"
                             : : "r"((uint32_t)l[i]), "r"((uint32_t)(l[i] >> 32)) : "memory");
            RES_D(r); record("cvt.d.l", r, get_fcsr(), 1);
            SET_RM(rm);
            __asm__ volatile("mtc1 %0, $f0\n\tcvt.d.s $f4, $f0" : : "r"(sv[i * 2]) : "memory");
            RES_D(r); record("cvt.d.s", r, get_fcsr(), 1);
        }
        puts_("cvt.from rm");
        putc_('0' + (int)rm);
        putc_(' ');
        hex(h, 16);
        putc_('\n');
    }
}

/* the sixteen conditions, each into FCC3, read by bc1t and by cfc1 */
static void compares(void)
{
    static const uint64_t cv[] = { 0x3ff0000000000000ull, 0x4000000000000000ull,
                                   0x7ff8000000000000ull, 0x8000000000000000ull, 0 };
    h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++) {
            uint32_t t[16], f[16];
            LOAD2_D(cv[i], cv[j]);
#define C(n, nm) SET_RM(0); __asm__ volatile("c." nm ".d $fcc3, $f0, $f2" ::: "memory"); \
            f[n] = get_fcsr(); __asm__ volatile(".set push\n\t.set noreorder\n\tli %0, 0\n\t" \
                "bc1t $fcc3, 1f\n\tnop\n\tli %0, 1\n1:\t.set pop" : "=&r"(t[n]));
            C(0, "f") C(1, "un") C(2, "eq") C(3, "ueq") C(4, "olt") C(5, "ult")
            C(6, "ole") C(7, "ule") C(8, "sf") C(9, "ngle") C(10, "seq") C(11, "ngl")
            C(12, "lt") C(13, "nge") C(14, "le") C(15, "ngt")
#undef C
            for (int k = 0; k < 16; k++) {
                mix(t[k]);
                mix(f[k] & 0x0203f07cu);
            }
        }
    line("compares", h);
}

static void moves(void)
{
    uint32_t a, b;
    /* the 64-bit register: mthc1 and mfhc1, and mfc1 its low half */
    __asm__ volatile("mtc1 %2, $f8\n\tmthc1 %3, $f8\n\tmfhc1 %0, $f8\n\tmfc1 %1, $f8"
                     : "=r"(a), "=r"(b) : "r"(0x11111111u), "r"(0x22222222u));
    line("mthc1/mfhc1", (uint64_t)a << 32 | b);
    /* movt and movf on FCC1, set by a compare */
    LOAD2_D(dv[2], dv[2]);
    __asm__ volatile("c.eq.d $fcc1, $f0, $f2");
    a = 5;
    b = 9;
    __asm__ volatile("movt %0, %1, $fcc1" : "+r"(a) : "r"(7u));
    __asm__ volatile("movf %0, %1, $fcc1" : "+r"(b) : "r"(3u));
    line("movt/movf", (uint64_t)a << 32 | b);
    /* movt.d */
    LOAD2_D(dv[4], dv[5]);
    __asm__ volatile("mov.d $f4, $f0\n\tmovt.d $f4, $f2, $fcc1");
    uint64_t r;
    RES_D(r);
    line("movt.d", r);
    /* ldc1/sdc1, ldxc1/sdxc1, lwxc1 */
    static volatile uint64_t mem[4] = { 0x0123456789abcdefull, 0, 0, 0 };
    uint32_t idx = 8;
    __asm__ volatile("ldc1 $f10, 0(%0)\n\tsdc1 $f10, 8(%0)\n\tldxc1 $f12, %1(%0)\n\t"
                     "sdxc1 $f12, %2(%0)\n\tlwxc1 $f14, %1(%0)\n\tswc1 $f14, 24(%0)"
                     : : "r"(mem), "r"(idx), "r"(16u) : "memory");
    line("ldc1/sdc1", mem[1]);
    line("ldxc1/sdxc1", mem[2]);
    line("lwxc1/swc1", mem[3]);
}

/* FCSR's Enables: a division by zero with Z enabled is the
 * floating-point exception, and $f4 is not written */
volatile uint32_t exc_cause, exc_fcsr, exc_n;
__asm__(
    "    .text\n"
    "    .set push\n"
    "    .set noreorder\n"
    "    .set noat\n"
    "    .globl fpe_handler\n"
    "fpe_handler:\n"
    "    lui   $27, %hi(exc_cause)\n"
    "    mfc0  $26, $13\n"
    "    sw    $26, %lo(exc_cause)($27)\n"
    "    lui   $27, %hi(exc_fcsr)\n"
    "    cfc1  $26, $31\n"
    "    sw    $26, %lo(exc_fcsr)($27)\n"
    "    ctc1  $0, $31\n"
    "    lui   $27, %hi(exc_n)\n"
    "    lw    $26, %lo(exc_n)($27)\n"
    "    addiu $26, $26, 1\n"
    "    sw    $26, %lo(exc_n)($27)\n"
    "    mfc0  $26, $14\n"
    "    addiu $26, $26, 4\n"
    "    mtc0  $26, $14\n"
    "    ehb\n"
    "    eret\n"
    "    nop\n"
    "    .set pop\n");
void fpe_handler(void);

static void exception(void)
{
    volatile uint32_t *v = (volatile uint32_t *)0x80000180u;
    uint32_t hd = (uint32_t)fpe_handler;
    v[0] = 0x3c1a0000u | (hd >> 16);
    v[1] = 0x375a0000u | (hd & 0xffffu);
    v[2] = 0x03400008u;
    v[3] = 0;
    uint64_t r;
    LOAD2_D(dv[2], dv[0]);
    __asm__ volatile("mtc1 $0, $f4\n\tmthc1 $0, $f4");
    __asm__ volatile("ctc1 %0, $31" : : "r"(1u << 10));      /* Enable Z */
    __asm__ volatile("div.d $f4, $f0, $f2" ::: "memory");
    RES_D(r);
    line("fpe taken", exc_n);
    line("fpe code", (exc_cause >> 2) & 31);
    line("fpe fcsr", exc_fcsr);
    line("fpe dest", r);
    /* V enabled: sqrt of -1 */
    LOAD2_D(dv[3], dv[0]);
    __asm__ volatile("ctc1 %0, $31" : : "r"(1u << 11));      /* Enable V */
    __asm__ volatile("sqrt.d $f4, $f0" ::: "memory");
    line("fpe V taken", exc_n);
    line("fpe V fcsr", exc_fcsr);
}

int main(void)
{
    uint32_t st;
    __asm__ volatile("mfc0 %0, $12" : "=r"(st));
    st |= 1u << 29 | 1u << 26;                  /* CU1, FR */
    st &= ~((1u << 22) | (1u << 2) | (1u << 1));
    __asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(st));
    add_d(); sub_d(); mul_d(); div_d();
    add_s(); mul_s(); div_s();
    sqrt_d(); recip_d(); rsqrt_d();
    cvt_s_d(); cvt_w_d(); cvt_l_d();
    round_w_d(); trunc_w_d(); ceil_w_d(); floor_w_d();
    abs_d(); neg_d();
    madd_d(); msub_d(); nmadd_d(); nmsub_d();
    conversions_from();
    compares();
    moves();
    exception();
    puts_("done\n");
    SOFTRES = 0x42;
    for (;;)
        ;
}

void _start(void)
{
    main();
}
