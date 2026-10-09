/* avr-isa.c -- AVR instruction edges for tests/golden/embsim-avr.sh: the
 * cases whose results are easy to get wrong, each printed, so that
 * EmbSim's output can be held to QEMU's (and to the record).
 *
 * The instructions are avr-isa.S's kernels; this sweeps them. Every
 * 8-bit arithmetic and logic instruction runs over every pair of a table
 * of operands -- each nibble's edges, the sign's, the ones that carry
 * and borrow out of bit 3 and bit 7 -- and over every operand for the
 * one-operand ones, with SREG coming in so that an
 * instruction that must leave a flag alone is seen to: 0x55 (C, N, S
 * and T set); and for the ones that read the carry, 0x03 (C and Z set)
 * and 0x7c (C and Z clear, the rest set), so ADC, SBC and CPC see both
 * carries and SBC, SBCI and CPC both Zs. Each prints one 16-bit hash of
 * its results and SREGs (-DVERBOSE: a line per case). Then the multiplies' R1:R0, ADIW and SBIW, the
 * skips over one- and two-word instructions, BST and BLD, every branch
 * under every SREG, X/Y/Z's pre-decrement, post-increment and
 * displacement, LPM's post-increment, the stack, and Timer/Counter1's
 * compare-match interrupt. Linked with the AVR harness and the corpus
 * driver's __embsim_end, so QEMU's plugin counts its instructions too
 * -- the skips among them. Built by EmbCC. The timer's wait spins as
 * long as QEMU's timer, which keeps the host's time, takes: the count is
 * compared on a build without it (-DNO_TIMER). */
#include <stdint.h>

void writec(int c);
void puts_(const char *s);
void __embsim_end(void);

uint8_t kbuf[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
volatile uint8_t kscratch, ticks;

static void hex(uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        writec("0123456789abcdef"[(v >> (4 * i)) & 15]);
}

static void line(const char *name, uint32_t v)
{
    puts_(name);
    writec(' ');
    hex(v, 8);
    writec('\n');
}

/* the hash: CRC-16 (CCITT's polynomial), so a byte that differs anywhere
 * changes it. (A multiply-and-xor hash in 16 bits kept only its last few
 * bytes, and Fletcher's sums mod 2^16 cancel on these tables' regular
 * differences: both printed one value for two different runs.) */
static uint16_t crc;
#define H ((uint32_t)crc)

static void hinit(void)
{
    crc = 0xffff;
}

static void mix(uint8_t x)
{
    crc ^= (uint16_t)(x << 8);
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                             : (uint16_t)(crc << 1);
}

static const uint8_t vals[] = {
    0x00, 0x01, 0x02, 0x07, 0x08, 0x09, 0x0f, 0x10, 0x11, 0x3f, 0x40, 0x55,
    0x7e, 0x7f, 0x80, 0x81, 0xaa, 0xc0, 0xef, 0xf0, 0xf1, 0xf8, 0xfe, 0xff,
};
/* the tables' lengths as enum constants: EmbCC at -Os divides a sizeof by
 * a sizeof at run time (a __udivsi3 call per loop test) */
enum { NV = sizeof vals / sizeof vals[0] };

/* the SREGs coming in: [0] alone, or all three for a carry-in form */
static const uint8_t sregs[3] = { 0x55, 0x03, 0x7c };

typedef uint16_t (*k8)(uint8_t a, uint8_t b, uint8_t s);
typedef uint32_t (*kmul)(uint8_t a, uint8_t b, uint8_t s);
typedef uint32_t (*k16)(uint16_t v, uint8_t s);

uint16_t k_add(uint8_t, uint8_t, uint8_t), k_adc(uint8_t, uint8_t, uint8_t);
uint16_t k_sub(uint8_t, uint8_t, uint8_t), k_sbc(uint8_t, uint8_t, uint8_t);
uint16_t k_and(uint8_t, uint8_t, uint8_t), k_or(uint8_t, uint8_t, uint8_t);
uint16_t k_eor(uint8_t, uint8_t, uint8_t), k_cp(uint8_t, uint8_t, uint8_t);
uint16_t k_cpc(uint8_t, uint8_t, uint8_t), k_mov(uint8_t, uint8_t, uint8_t);
uint16_t k_subi_00(uint8_t, uint8_t, uint8_t), k_subi_01(uint8_t, uint8_t, uint8_t);
uint16_t k_subi_80(uint8_t, uint8_t, uint8_t), k_subi_ff(uint8_t, uint8_t, uint8_t);
uint16_t k_sbci_00(uint8_t, uint8_t, uint8_t), k_sbci_01(uint8_t, uint8_t, uint8_t);
uint16_t k_sbci_80(uint8_t, uint8_t, uint8_t), k_sbci_ff(uint8_t, uint8_t, uint8_t);
uint16_t k_cpi_00(uint8_t, uint8_t, uint8_t), k_cpi_7f(uint8_t, uint8_t, uint8_t);
uint16_t k_cpi_80(uint8_t, uint8_t, uint8_t), k_cpi_ff(uint8_t, uint8_t, uint8_t);
uint16_t k_andi_0f(uint8_t, uint8_t, uint8_t), k_andi_80(uint8_t, uint8_t, uint8_t);
uint16_t k_ori_01(uint8_t, uint8_t, uint8_t), k_ori_80(uint8_t, uint8_t, uint8_t);
uint16_t k_ldi_a5(uint8_t, uint8_t, uint8_t);
uint16_t k_com(uint8_t, uint8_t, uint8_t), k_neg(uint8_t, uint8_t, uint8_t);
uint16_t k_swap(uint8_t, uint8_t, uint8_t), k_inc(uint8_t, uint8_t, uint8_t);
uint16_t k_dec(uint8_t, uint8_t, uint8_t), k_asr(uint8_t, uint8_t, uint8_t);
uint16_t k_lsr(uint8_t, uint8_t, uint8_t), k_ror(uint8_t, uint8_t, uint8_t);
uint16_t k_lsl(uint8_t, uint8_t, uint8_t), k_rol(uint8_t, uint8_t, uint8_t);
uint16_t k_tst(uint8_t, uint8_t, uint8_t), k_clr(uint8_t, uint8_t, uint8_t);
uint16_t k_ser(uint8_t, uint8_t, uint8_t);
uint32_t k_mul(uint8_t, uint8_t, uint8_t), k_muls(uint8_t, uint8_t, uint8_t);
uint32_t k_mulsu(uint8_t, uint8_t, uint8_t), k_fmul(uint8_t, uint8_t, uint8_t);
uint32_t k_fmuls(uint8_t, uint8_t, uint8_t), k_fmulsu(uint8_t, uint8_t, uint8_t);
uint32_t k_adiw_0(uint16_t, uint8_t), k_adiw_1(uint16_t, uint8_t);
uint32_t k_adiw_63(uint16_t, uint8_t), k_sbiw_0(uint16_t, uint8_t);
uint32_t k_sbiw_1(uint16_t, uint8_t), k_sbiw_63(uint16_t, uint8_t);
uint8_t k_cpse(uint8_t, uint8_t), k_sbrx(uint8_t), k_sbix(void), k_sbit(void);
uint16_t k_bstbld(uint8_t, uint8_t, uint8_t), k_branches(uint8_t);
uint32_t k_ldx(void), k_lddz(void), k_lpm(void), k_pushpop(uint8_t);
uint8_t k_sty(void);
uint32_t k_movw(uint16_t);
void k_sei(void), k_cli(void);

/* two operands (b swept), or one (b 0); 3 for the carry-in forms, which
 * run under each of the three SREGs */
static const struct { const char *name; k8 fn; int two; } ops[] = {
    { "add", k_add, 1 }, { "adc", k_adc, 3 }, { "sub", k_sub, 1 },
    { "sbc", k_sbc, 3 }, { "and", k_and, 1 }, { "or", k_or, 1 },
    { "eor", k_eor, 1 }, { "cp", k_cp, 1 }, { "cpc", k_cpc, 3 },
    { "mov", k_mov, 1 }, { "bst-bld", k_bstbld, 1 },
    { "subi-00", k_subi_00, 0 }, { "subi-01", k_subi_01, 0 },
    { "subi-80", k_subi_80, 0 }, { "subi-ff", k_subi_ff, 0 },
    { "sbci-00", k_sbci_00, 0 }, { "sbci-01", k_sbci_01, 0 },
    { "sbci-80", k_sbci_80, 0 }, { "sbci-ff", k_sbci_ff, 0 },
    { "cpi-00", k_cpi_00, 0 }, { "cpi-7f", k_cpi_7f, 0 },
    { "cpi-80", k_cpi_80, 0 }, { "cpi-ff", k_cpi_ff, 0 },
    { "andi-0f", k_andi_0f, 0 }, { "andi-80", k_andi_80, 0 },
    { "ori-01", k_ori_01, 0 }, { "ori-80", k_ori_80, 0 },
    { "ldi-a5", k_ldi_a5, 0 },
    { "com", k_com, 0 }, { "neg", k_neg, 0 }, { "swap", k_swap, 0 },
    { "inc", k_inc, 0 }, { "dec", k_dec, 0 }, { "asr", k_asr, 0 },
    { "lsr", k_lsr, 0 }, { "ror", k_ror, 0 }, { "lsl", k_lsl, 0 },
    { "rol", k_rol, 0 }, { "tst", k_tst, 0 }, { "clr", k_clr, 0 },
    { "ser", k_ser, 0 },
};

enum { NOPS = sizeof ops / sizeof ops[0] };

static const struct { const char *name; kmul fn; } muls[] = {
    { "mul", k_mul }, { "muls", k_muls }, { "mulsu", k_mulsu },
    { "fmul", k_fmul }, { "fmuls", k_fmuls }, { "fmulsu", k_fmulsu },
};

enum { NMULS = sizeof muls / sizeof muls[0] };

static const struct { const char *name; k16 fn; } w16[] = {
    { "adiw-0", k_adiw_0 }, { "adiw-1", k_adiw_1 }, { "adiw-63", k_adiw_63 },
    { "sbiw-0", k_sbiw_0 }, { "sbiw-1", k_sbiw_1 }, { "sbiw-63", k_sbiw_63 },
};

enum { NW16 = sizeof w16 / sizeof w16[0] };

#ifdef VERBOSE
static void verbose(const char *name, unsigned s, unsigned a, unsigned b,
                    uint32_t r)
{
    puts_(name); writec(' '); hex(s, 2); writec(' '); hex(a, 4); writec(' ');
    hex(b, 2); writec(' '); hex(r, 8); writec('\n');
}
#else
#define verbose(name, s, a, b, r) ((void)0)
#endif

static void sweeps(void)
{
    for (unsigned k = 0; k < NOPS; k++) {
        hinit();
        for (int si = 0; si < (ops[k].two == 1 ? 1 : 3); si++)
            for (unsigned a = 0; a < (ops[k].two ? NV : 256u); a++)
                for (unsigned b = 0; b < (ops[k].two ? NV : 1u); b++) {
                    uint16_t r = ops[k].fn(ops[k].two ? vals[a] : (uint8_t)a,
                                           vals[b], sregs[si]);
                    mix((uint8_t)r);
                    mix((uint8_t)(r >> 8));
                    verbose(ops[k].name, sregs[si], a, b, r);
                }
        line(ops[k].name, H);
    }
    for (unsigned k = 0; k < NMULS; k++) {
        hinit();
        for (unsigned a = 0; a < NV; a++)
            for (unsigned b = 0; b < NV; b++) {
                uint32_t r = muls[k].fn(vals[a], vals[b], 0x7f);
                mix((uint8_t)r);
                mix((uint8_t)(r >> 8));
                mix((uint8_t)(r >> 16));
                verbose(muls[k].name, 0x7f, a, b, r);
            }
        line(muls[k].name, H);
    }
    for (unsigned k = 0; k < NW16; k++) {
        hinit();
        for (int si = 0; si < 3; si++)
            for (uint32_t i = 0; i < 0x10000; i += 0x7ff)
                for (int e = 0; e < 2; e++) {
                    /* a stride through the values, and its mirror near
                     * 0x7fff and 0x8000 */
                    uint16_t v = (uint16_t)(e ? i ^ 0x7fc0 : i);
                    uint32_t r = w16[k].fn(v, sregs[si]);
                    mix((uint8_t)r);
                    mix((uint8_t)(r >> 8));
                    mix((uint8_t)(r >> 16));
                    verbose(w16[k].name, sregs[si], v, 0, r);
                }
        line(w16[k].name, H);
    }
}

static void skips(void)
{
    hinit();
    for (unsigned a = 0; a < 256; a += 3)
        for (unsigned b = 0; b < 256; b += 5)
            mix(k_cpse((uint8_t)a, (uint8_t)(b == 255 ? a : b)));
    line("cpse", H);
    hinit();
    for (unsigned a = 0; a < 256; a++)
        mix(k_sbrx((uint8_t)a));
    line("sbrc-sbrs", H);
    line("sbic-sbis-clear", k_sbix());
    hinit();
    for (unsigned s = 0; s < 256; s++) {
        uint16_t r = k_branches((uint8_t)s);
        mix((uint8_t)r);
        mix((uint8_t)(r >> 8));
    }
    line("brbs-brbc", H);
}

static void memory(void)
{
    line("ld-x", k_ldx());
    line("st-y", k_sty());
    line("st-y-bytes", (uint32_t)kbuf[4] << 16 | (uint32_t)kbuf[5] << 8 | kbuf[6]);
    line("ldd-std-z", k_lddz());
    line("kbuf14", kbuf[14]);
    line("lpm", k_lpm());
    line("push-pop", k_pushpop(0x77));
    line("movw-regfile", k_movw(0x1234));
}

static void timer(void)
{
    volatile uint8_t *const TCCR1A = (volatile uint8_t *)0x80;
    volatile uint8_t *const TCCR1B = (volatile uint8_t *)0x81;
    volatile uint8_t *const OCR1AL = (volatile uint8_t *)0x88;
    volatile uint8_t *const OCR1AH = (volatile uint8_t *)0x89;
    volatile uint8_t *const TIMSK1 = (volatile uint8_t *)0x6f;
    volatile uint8_t *const TIFR1 = (volatile uint8_t *)0x36;
    /* (TIFR1 is not written: on QEMU a write sets the flags written,
     * where the part clears them) */
    *OCR1AH = 0;
    *OCR1AL = 200;
    *TCCR1A = 0;
    *TIMSK1 = 0x02;                         /* OCIE1A */
    *TCCR1B = 0x09;                         /* CTC on OCR1A, clock /1 */
    k_sei();
    while (ticks < 5)
        ;
    k_cli();
    line("timer1-compa", ticks);
    /* SBIC and SBIS on a bit that is set: OCF1A, at the next match with
     * its interrupt off */
    *TIMSK1 = 0;
    while (!(*TIFR1 & 2))
        ;
    *TCCR1B = 0;
    line("sbic-sbis-set", k_sbit());
}

int main(void)
{
    sweeps();
    skips();
    memory();
#ifndef NO_TIMER
    timer();
#endif
    __embsim_end();
    return 0;
}
