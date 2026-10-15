/* mips-dsp.c -- the DSP ASE's edges (Revision 2, MIPS32 encodings) for
 * tests/golden/embsim-mips.sh, run on QEMU's malta with a 74Kf (DSPr2)
 * and on EmbSim's malta with the pic32mz core, a PIC32MZ's microAptiv.
 * The results are printed, so EmbSim's output can be held to QEMU's (and
 * to the record).
 *
 * Built by clang for mipsel (mips32r2, -mdspr2), linked by embld at
 * malta's RAM; _start (embld's -Tstack stub) calls main, which turns the
 * DSP on (Status.MX) and resets the board when done.
 *
 * Each instruction runs over a table of operands (the lanes' extremes,
 * -1, the Q15 and Q31 -1 and nearly 1, mixed lanes) from three states of
 * DSPControl (its carry, ccond, pos and scount), and the accumulator
 * instructions from a table of accumulators too; one FNV-1a hash per
 * instruction is printed of every result and of DSPControl after it (its
 * overflow flags, carry, ccond, pos, EFI). Also: rddsp's and wrdsp's
 * masks, bposge32, the indexed loads, the DSP instructions with
 * Status.MX clear (DSP state disabled, ExcCode 26) and an encoding of a
 * DSP group no instruction has (Reserved Instruction). Built with
 * -DVERBOSE it prints every result instead. */
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
static void line(const char *name, uint64_t v, int digits)
{
    puts_(name);
    putc_(' ');
    hex(v, digits);
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
static void start(void) { h = 0xcbf29ce484222325ull; }
static void record(uint64_t r, uint32_t dsp)
{
#ifdef VERBOSE
    hex(r, 16);
    putc_(' ');
    hex(dsp, 8);
    putc_('\n');
#endif
    mix(r);
    mix(dsp);
}
static void report(const char *name) { line(name, h, 16); }

static const uint32_t val[] = {
    0x00000000u, 0x00000001u, 0xffffffffu, 0x7fffffffu,
    0x80000000u, 0x7fff7fffu, 0x80008000u, 0x80007fffu,
    0x00ff00ffu, 0xff00ff00u, 0x7f80017fu, 0x807f80ffu,
    0x12345678u, 0xfedcba98u, 0x40004000u, 0x0001ffffu,
};
#define NV (sizeof val / sizeof val[0])

/* DSPControl before: nothing; ccond 1010, the carry, scount 37, pos 20;
 * ccond 0101, the carry, pos 31 */
static const uint32_t dsp0[] = { 0x00000000u, 0x0a003294u, 0x0500201fu };
#define ND (sizeof dsp0 / sizeof dsp0[0])

static const uint64_t accv[] = {
    0x0000000000000000ull, 0x0000000000000001ull, 0x000000007fffffffull,
    0x0000000080000000ull, 0xffffffff80000000ull, 0x7fffffffffffffffull,
    0x8000000000000000ull, 0xffffffffffffffffull, 0x123456789abcdef0ull,
    0x00000000ffffffffull, 0xfffffffe00000001ull, 0x0000000100000000ull,
};
#define NA (sizeof accv / sizeof accv[0])

static uint32_t rddsp(void)
{
    uint32_t v;
    __asm__ volatile("rddsp %0, 0x3f" : "=r"(v));
    return v;
}
static void wrdsp(uint32_t v) { __asm__ volatile("wrdsp %0, 0x3f" : : "r"(v)); }

/* rd = op rs, rt */
#define RRR(fn, insn)                                                   \
    static void fn(void)                                                \
    {                                                                   \
        start();                                                        \
        for (unsigned d = 0; d < ND; d++)                               \
            for (unsigned i = 0; i < NV; i++)                           \
                for (unsigned j = 0; j < NV; j++) {                     \
                    uint32_t r;                                         \
                    wrdsp(dsp0[d]);                                     \
                    __asm__ volatile(insn " %0, %1, %2"                 \
                                     : "=r"(r) : "r"(val[i]), "r"(val[j])); \
                    record(r, rddsp());                                 \
                }                                                       \
        report(insn);                                                   \
    }

/* rd = op rt, or rd = op rt, imm */
#define RR(fn, insn, name)                                              \
    static void fn(void)                                                \
    {                                                                   \
        start();                                                        \
        for (unsigned d = 0; d < ND; d++)                               \
            for (unsigned i = 0; i < NV; i++) {                         \
                uint32_t r;                                             \
                wrdsp(dsp0[d]);                                         \
                __asm__ volatile(insn : "=r"(r) : "r"(val[i]));          \
                record(r, rddsp());                                     \
            }                                                           \
        report(name);                                                   \
    }

/* rt = op rt, rs[, imm]: rt read and written */
#define RMW(fn, insn, name)                                             \
    static void fn(void)                                                \
    {                                                                   \
        start();                                                        \
        for (unsigned d = 0; d < ND; d++)                               \
            for (unsigned i = 0; i < NV; i++)                           \
                for (unsigned j = 0; j < NV; j++) {                     \
                    uint32_t r = val[j];                                \
                    wrdsp(dsp0[d]);                                     \
                    __asm__ volatile(insn : "+r"(r) : "r"(val[i]));      \
                    record(r, rddsp());                                 \
                }                                                       \
        report(name);                                                   \
    }

/* the compares: only DSPControl */
#define CMP(fn, insn)                                                   \
    static void fn(void)                                                \
    {                                                                   \
        start();                                                        \
        for (unsigned d = 0; d < ND; d++)                               \
            for (unsigned i = 0; i < NV; i++)                           \
                for (unsigned j = 0; j < NV; j++) {                     \
                    wrdsp(dsp0[d]);                                     \
                    __asm__ volatile(insn " %0, %1" : : "r"(val[i]), "r"(val[j])); \
                    record(0, rddsp());                                 \
                }                                                       \
        report(insn);                                                   \
    }

static void acc_put(uint64_t v)
{
    __asm__ volatile("mthi %0, $ac1\n\tmtlo %1, $ac1"
                     : : "r"((uint32_t)(v >> 32)), "r"((uint32_t)v));
}
static uint64_t acc_get(void)
{
    uint32_t hi, lo;
    __asm__ volatile("mfhi %0, $ac1\n\tmflo %1, $ac1" : "=r"(hi), "=r"(lo));
    return (uint64_t)hi << 32 | lo;
}

/* $ac1 = op $ac1, rs, rt */
#define ACC(fn, insn)                                                   \
    static void fn(void)                                                \
    {                                                                   \
        start();                                                        \
        for (unsigned a = 0; a < NA; a++)                               \
            for (unsigned i = 0; i < NV; i++)                           \
                for (unsigned j = 0; j < NV; j++) {                     \
                    wrdsp(dsp0[(a + i + j) % ND]);                      \
                    acc_put(accv[a]);                                   \
                    __asm__ volatile(insn " $ac1, %0, %1"               \
                                     : : "r"(val[i]), "r"(val[j]));     \
                    record(acc_get(), rddsp());                         \
                }                                                       \
        report(insn);                                                   \
    }

/* rt = op $ac1, imm or rs; pos for extp from the table of states */
static const uint32_t posv[] = { 0, 5, 15, 30, 31, 32, 40, 63 };
#define NP (sizeof posv / sizeof posv[0])
#define EXT(fn, insn, name)                                             \
    static void fn(void)                                                \
    {                                                                   \
        start();                                                        \
        for (unsigned p = 0; p < NP; p++)                               \
            for (unsigned a = 0; a < NA; a++)                           \
                for (unsigned i = 0; i < NV; i++) {                     \
                    uint32_t r;                                         \
                    wrdsp(posv[p] | (p & 1) << 14);                     \
                    acc_put(accv[a]);                                   \
                    __asm__ volatile(insn : "=r"(r) : "r"(val[i]));      \
                    record(r, rddsp());                                 \
                    record(acc_get(), 0);                               \
                }                                                       \
        report(name);                                                   \
    }

/* ---- the instructions ------------------------------------------------- */

RRR(addu_qb, "addu.qb")         RRR(addu_s_qb, "addu_s.qb")
RRR(subu_qb, "subu.qb")         RRR(subu_s_qb, "subu_s.qb")
RRR(addu_ph, "addu.ph")         RRR(addu_s_ph, "addu_s.ph")
RRR(subu_ph, "subu.ph")         RRR(subu_s_ph, "subu_s.ph")
RRR(addq_ph, "addq.ph")         RRR(addq_s_ph, "addq_s.ph")
RRR(subq_ph, "subq.ph")         RRR(subq_s_ph, "subq_s.ph")
RRR(addq_s_w, "addq_s.w")       RRR(subq_s_w, "subq_s.w")
RRR(addsc, "addsc")             RRR(addwc, "addwc")
RRR(modsub, "modsub")
RRR(adduh_qb, "adduh.qb")       RRR(adduh_r_qb, "adduh_r.qb")
RRR(subuh_qb, "subuh.qb")       RRR(subuh_r_qb, "subuh_r.qb")
RRR(addqh_ph, "addqh.ph")       RRR(addqh_r_ph, "addqh_r.ph")
RRR(subqh_ph, "subqh.ph")       RRR(subqh_r_ph, "subqh_r.ph")
RRR(addqh_w, "addqh.w")         RRR(addqh_r_w, "addqh_r.w")
RRR(subqh_w, "subqh.w")         RRR(subqh_r_w, "subqh_r.w")
RRR(muleu_s_ph_qbl, "muleu_s.ph.qbl") RRR(muleu_s_ph_qbr, "muleu_s.ph.qbr")
RRR(mulq_rs_ph, "mulq_rs.ph")   RRR(mulq_s_ph, "mulq_s.ph")
RRR(muleq_s_w_phl, "muleq_s.w.phl") RRR(muleq_s_w_phr, "muleq_s.w.phr")
RRR(mul_ph, "mul.ph")           RRR(mul_s_ph, "mul_s.ph")
RRR(mulq_s_w, "mulq_s.w")       RRR(mulq_rs_w, "mulq_rs.w")
RRR(precrq_qb_ph, "precrq.qb.ph") RRR(precr_qb_ph, "precr.qb.ph")
RRR(precrq_ph_w, "precrq.ph.w") RRR(precrq_rs_ph_w, "precrq_rs.ph.w")
RRR(precrqu_s_qb_ph, "precrqu_s.qb.ph")
RRR(packrl_ph, "packrl.ph")
RRR(pick_qb, "pick.qb")         RRR(pick_ph, "pick.ph")
RRR(cmpgu_eq_qb, "cmpgu.eq.qb") RRR(cmpgu_lt_qb, "cmpgu.lt.qb")
RRR(cmpgu_le_qb, "cmpgu.le.qb")
RRR(cmpgdu_eq_qb, "cmpgdu.eq.qb") RRR(cmpgdu_lt_qb, "cmpgdu.lt.qb")
RRR(cmpgdu_le_qb, "cmpgdu.le.qb")
CMP(cmpu_eq_qb, "cmpu.eq.qb")   CMP(cmpu_lt_qb, "cmpu.lt.qb")
CMP(cmpu_le_qb, "cmpu.le.qb")
CMP(cmp_eq_ph, "cmp.eq.ph")     CMP(cmp_lt_ph, "cmp.lt.ph")
CMP(cmp_le_ph, "cmp.le.ph")
/* the variable shifts: rd = op rt, rs */
#define SHV(fn, insn)                                                   \
    static void fn(void)                                                \
    {                                                                   \
        start();                                                        \
        for (unsigned d = 0; d < ND; d++)                               \
            for (unsigned i = 0; i < NV; i++)                           \
                for (unsigned j = 0; j < NV; j++) {                     \
                    uint32_t r;                                         \
                    wrdsp(dsp0[d]);                                     \
                    __asm__ volatile(insn " %0, %1, %2"                 \
                                     : "=r"(r) : "r"(val[i]), "r"(val[j] + j)); \
                    record(r, rddsp());                                 \
                }                                                       \
        report(insn);                                                   \
    }
SHV(shllv_qb, "shllv.qb")       SHV(shrlv_qb, "shrlv.qb")
SHV(shrav_qb, "shrav.qb")       SHV(shrav_r_qb, "shrav_r.qb")
SHV(shllv_ph, "shllv.ph")       SHV(shllv_s_ph, "shllv_s.ph")
SHV(shrlv_ph, "shrlv.ph")       SHV(shrav_ph, "shrav.ph")
SHV(shrav_r_ph, "shrav_r.ph")   SHV(shllv_s_w, "shllv_s.w")
SHV(shrav_r_w, "shrav_r.w")

/* the immediate shifts, at four amounts each */
#define SH4(fn, op, a, b, c, d)                                         \
    RR(fn##_a, op " %0, %1, " #a, op " " #a)                             \
    RR(fn##_b, op " %0, %1, " #b, op " " #b)                             \
    RR(fn##_c, op " %0, %1, " #c, op " " #c)                             \
    RR(fn##_d, op " %0, %1, " #d, op " " #d)                             \
    static void fn(void) { fn##_a(); fn##_b(); fn##_c(); fn##_d(); }
SH4(shll_qb, "shll.qb", 0, 1, 4, 7)
SH4(shrl_qb, "shrl.qb", 0, 1, 4, 7)
SH4(shra_qb, "shra.qb", 0, 1, 4, 7)
SH4(shra_r_qb, "shra_r.qb", 0, 1, 4, 7)
SH4(shll_ph, "shll.ph", 0, 1, 8, 15)
SH4(shll_s_ph, "shll_s.ph", 0, 1, 8, 15)
SH4(shrl_ph, "shrl.ph", 0, 1, 8, 15)
SH4(shra_ph, "shra.ph", 0, 1, 8, 15)
SH4(shra_r_ph, "shra_r.ph", 0, 1, 8, 15)
SH4(shll_s_w, "shll_s.w", 0, 1, 16, 31)
SH4(shra_r_w, "shra_r.w", 0, 1, 16, 31)

RR(absq_s_qb, "absq_s.qb %0, %1", "absq_s.qb")
RR(absq_s_ph, "absq_s.ph %0, %1", "absq_s.ph")
RR(absq_s_w, "absq_s.w %0, %1", "absq_s.w")
RR(raddu_w_qb, "raddu.w.qb %0, %1", "raddu.w.qb")
RR(preceq_w_phl, "preceq.w.phl %0, %1", "preceq.w.phl")
RR(preceq_w_phr, "preceq.w.phr %0, %1", "preceq.w.phr")
RR(precequ_ph_qbl, "precequ.ph.qbl %0, %1", "precequ.ph.qbl")
RR(precequ_ph_qbr, "precequ.ph.qbr %0, %1", "precequ.ph.qbr")
RR(precequ_ph_qbla, "precequ.ph.qbla %0, %1", "precequ.ph.qbla")
RR(precequ_ph_qbra, "precequ.ph.qbra %0, %1", "precequ.ph.qbra")
RR(preceu_ph_qbl, "preceu.ph.qbl %0, %1", "preceu.ph.qbl")
RR(preceu_ph_qbr, "preceu.ph.qbr %0, %1", "preceu.ph.qbr")
RR(preceu_ph_qbla, "preceu.ph.qbla %0, %1", "preceu.ph.qbla")
RR(preceu_ph_qbra, "preceu.ph.qbra %0, %1", "preceu.ph.qbra")
RR(bitrev, "bitrev %0, %1", "bitrev")
RR(replv_qb, "replv.qb %0, %1", "replv.qb")
RR(replv_ph, "replv.ph %0, %1", "replv.ph")

/* the bit inserts and the appends: rt read and written */
RMW(precr_sra_ph_w_0, "precr_sra.ph.w %0, %1, 0", "precr_sra.ph.w 0")
RMW(precr_sra_ph_w_1, "precr_sra.ph.w %0, %1, 1", "precr_sra.ph.w 1")
RMW(precr_sra_ph_w_31, "precr_sra.ph.w %0, %1, 31", "precr_sra.ph.w 31")
RMW(precr_sra_r_ph_w_0, "precr_sra_r.ph.w %0, %1, 0", "precr_sra_r.ph.w 0")
RMW(precr_sra_r_ph_w_1, "precr_sra_r.ph.w %0, %1, 1", "precr_sra_r.ph.w 1")
RMW(precr_sra_r_ph_w_16, "precr_sra_r.ph.w %0, %1, 16", "precr_sra_r.ph.w 16")
RMW(append_0, "append %0, %1, 0", "append 0")
RMW(append_1, "append %0, %1, 1", "append 1")
RMW(append_31, "append %0, %1, 31", "append 31")
RMW(prepend_0, "prepend %0, %1, 0", "prepend 0")
RMW(prepend_1, "prepend %0, %1, 1", "prepend 1")
RMW(prepend_31, "prepend %0, %1, 31", "prepend 31")
RMW(balign_0, "balign %0, %1, 0", "balign 0")
RMW(balign_1, "balign %0, %1, 1", "balign 1")
RMW(balign_2, "balign %0, %1, 2", "balign 2")
RMW(balign_3, "balign %0, %1, 3", "balign 3")

static void repl(void)
{
    uint32_t r[6];
    __asm__ volatile("repl.qb %0, 0\n\trepl.qb %1, 0x80\n\trepl.qb %2, 0xff"
                     : "=r"(r[0]), "=r"(r[1]), "=r"(r[2]));
    __asm__ volatile("repl.ph %0, 0\n\trepl.ph %1, -512\n\trepl.ph %2, 511"
                     : "=r"(r[3]), "=r"(r[4]), "=r"(r[5]));
    start();
    for (int i = 0; i < 6; i++)
        record(r[i], 0);
    report("repl");
}

/* insv: pos and scount from DSPControl, the field within the word */
static void insv(void)
{
    static const uint32_t pos[] = { 0, 1, 8, 20, 31 };
    static const uint32_t size[] = { 0, 1, 7, 12, 32 };
    start();
    for (unsigned p = 0; p < 5; p++)
        for (unsigned s = 0; s < 5; s++) {
            if (pos[p] + size[s] > 32)
                continue;
            for (unsigned i = 0; i < NV; i++)
                for (unsigned j = 0; j < NV; j += 3) {
                    uint32_t r = val[j];
                    wrdsp(pos[p] | size[s] << 7);
                    __asm__ volatile("insv %0, %1" : "+r"(r) : "r"(val[i]));
                    record(r, rddsp());
                }
        }
    report("insv");
}

ACC(dpau_h_qbl, "dpau.h.qbl")   ACC(dpau_h_qbr, "dpau.h.qbr")
ACC(dpsu_h_qbl, "dpsu.h.qbl")   ACC(dpsu_h_qbr, "dpsu.h.qbr")
ACC(dpa_w_ph, "dpa.w.ph")       ACC(dpax_w_ph, "dpax.w.ph")
ACC(dps_w_ph, "dps.w.ph")       ACC(dpsx_w_ph, "dpsx.w.ph")
ACC(dpaq_s_w_ph, "dpaq_s.w.ph") ACC(dpaqx_s_w_ph, "dpaqx_s.w.ph")
ACC(dpsq_s_w_ph, "dpsq_s.w.ph") ACC(dpsqx_s_w_ph, "dpsqx_s.w.ph")
ACC(dpaqx_sa_w_ph, "dpaqx_sa.w.ph") ACC(dpsqx_sa_w_ph, "dpsqx_sa.w.ph")
ACC(dpaq_sa_l_w, "dpaq_sa.l.w") ACC(dpsq_sa_l_w, "dpsq_sa.l.w")
ACC(mulsaq_s_w_ph, "mulsaq_s.w.ph") ACC(mulsa_w_ph, "mulsa.w.ph")
ACC(maq_s_w_phl, "maq_s.w.phl") ACC(maq_s_w_phr, "maq_s.w.phr")
ACC(maq_sa_w_phl, "maq_sa.w.phl") ACC(maq_sa_w_phr, "maq_sa.w.phr")
ACC(madd_ac, "madd")            ACC(maddu_ac, "maddu")
ACC(msub_ac, "msub")            ACC(msubu_ac, "msubu")
ACC(mult_ac, "mult")            ACC(multu_ac, "multu")

/* the extracts: at a few shifts, and by a register */
#define EXT4(fn, op, a, b, c, d)                                        \
    EXT(fn##_a, op " %0, $ac1, " #a, op " " #a)                          \
    EXT(fn##_b, op " %0, $ac1, " #b, op " " #b)                          \
    EXT(fn##_c, op " %0, $ac1, " #c, op " " #c)                          \
    EXT(fn##_d, op " %0, $ac1, " #d, op " " #d)                          \
    static void fn(void) { fn##_a(); fn##_b(); fn##_c(); fn##_d(); }
EXT4(extr_w, "extr.w", 0, 1, 15, 31)
EXT4(extr_r_w, "extr_r.w", 0, 1, 15, 31)
EXT4(extr_rs_w, "extr_rs.w", 0, 1, 15, 31)
EXT4(extr_s_h, "extr_s.h", 0, 1, 16, 31)
EXT4(extp, "extp", 0, 1, 7, 31)
EXT4(extpdp, "extpdp", 0, 1, 7, 31)
EXT(extrv_w, "extrv.w %0, $ac1, %1", "extrv.w")
EXT(extrv_r_w, "extrv_r.w %0, $ac1, %1", "extrv_r.w")
EXT(extrv_rs_w, "extrv_rs.w %0, $ac1, %1", "extrv_rs.w")
EXT(extrv_s_h, "extrv_s.h %0, $ac1, %1", "extrv_s.h")
EXT(extpv, "extpv %0, $ac1, %1", "extpv")
EXT(extpdpv, "extpdpv %0, $ac1, %1", "extpdpv")
/* shilo, shilov, mthlip: the accumulator itself */
EXT(shilo_a, "shilo $ac1, -32\n\tmove %0, $0", "shilo -32")
EXT(shilo_b, "shilo $ac1, -1\n\tmove %0, $0", "shilo -1")
EXT(shilo_c, "shilo $ac1, 1\n\tmove %0, $0", "shilo 1")
EXT(shilo_d, "shilo $ac1, 31\n\tmove %0, $0", "shilo 31")
EXT(shilov, "shilov $ac1, %1\n\tmove %0, $0", "shilov")
EXT(mthlip, "mthlip %1, $ac1\n\tmove %0, $0", "mthlip")

/* the other accumulators: the flag each sets is its own (16 + ac) */
static void other_acc(void)
{
    uint32_t d[4];
    for (int k = 0; k < 4; k++) {
        wrdsp(0);
        uint32_t m = 0x80008000u;
        switch (k) {
        case 0: __asm__ volatile("dpaq_s.w.ph $ac0, %0, %0" : : "r"(m)); break;
        case 1: __asm__ volatile("dpaq_s.w.ph $ac1, %0, %0" : : "r"(m)); break;
        case 2: __asm__ volatile("dpaq_s.w.ph $ac2, %0, %0" : : "r"(m)); break;
        default: __asm__ volatile("dpaq_s.w.ph $ac3, %0, %0" : : "r"(m)); break;
        }
        d[k] = rddsp();
    }
    for (int k = 0; k < 4; k++)
        line("dpaq_s.w.ph $ac flags", d[k], 8);
}

/* rddsp's and wrdsp's masks, one field at a time */
#define WR1(m)                                                          \
    do {                                                                \
        wrdsp(0);                                                       \
        __asm__ volatile("wrdsp %0, " #m : : "r"(0xffffffffu));         \
        line("wrdsp " #m, rddsp(), 8);                                  \
        wrdsp(0xffffffffu);                                             \
        __asm__ volatile("rddsp %0, " #m : "=r"(r));                    \
        line("rddsp " #m, r, 8);                                        \
    } while (0)
static void masks(void)
{
    uint32_t r;
    WR1(1); WR1(2); WR1(4); WR1(8); WR1(16); WR1(32); WR1(0x3f);
}

static void bposge32(void)
{
    static const uint32_t pos[] = { 0, 31, 32, 33, 63 };
    for (int p = 0; p < 5; p++) {
        uint32_t r;
        wrdsp(pos[p]);
        __asm__ volatile(".set push\n\t.set noreorder\n\t"
                         "bposge32 1f\n\tli %0, 1\n\tli %0, 0\n1:\n\t.set pop"
                         : "=r"(r));
        line("bposge32", r, 1);
    }
}

static void loads(void)
{
    static const uint32_t mem[2] = { 0x80ff7f01u, 0x12345678u };
    uint32_t r[6];
    uint32_t base = (uint32_t)mem;
    __asm__ volatile("lwx %0, %3(%4)\n\tlhx %1, %5(%4)\n\tlbux %2, %6(%4)"
                     : "=&r"(r[0]), "=&r"(r[1]), "=&r"(r[2])
                     : "r"(4), "r"(base), "r"(2), "r"(1));
    __asm__ volatile("lwx %0, %3(%4)\n\tlhx %1, %5(%4)\n\tlbux %2, %6(%4)"
                     : "=&r"(r[3]), "=&r"(r[4]), "=&r"(r[5])
                     : "r"(0), "r"(base), "r"(6), "r"(3));
    for (int i = 0; i < 6; i++)
        line("lx", r[i], 8);
}

/* ---- exceptions ------------------------------------------------------- */

/* Cause, and whether one was taken; the handler resumes past the
 * instruction */
volatile uint32_t exc_info[2];

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
    "    li    $26, 1\n"
    "    sw    $26, 4($27)\n"
    "    mfc0  $26, $14\n"
    "    addiu $26, $26, 4\n"
    "    mtc0  $26, $14\n"
    "    ehb\n"
    "    eret\n"
    "    nop\n"
    "    .set pop\n");
void exc_handler(void);

static void vector(uint32_t at, void (*to)(void))
{
    volatile uint32_t *v = (volatile uint32_t *)at;
    uint32_t a = (uint32_t)to;
    v[0] = 0x3c1a0000u | (a >> 16);                 /* lui  k0, %hi */
    v[1] = 0x375a0000u | (a & 0xffffu);             /* ori  k0, k0, %lo */
    v[2] = 0x03400008u;                             /* jr   k0 */
    v[3] = 0;                                       /* nop */
}

static void exc_line(const char *name)
{
    puts_(name);
    if (!exc_info[1]) {
        puts_(" none\n");
    } else {
        puts_(" code ");
        hex((exc_info[0] >> 2) & 31, 2);
        putc_('\n');
    }
    exc_info[1] = 0;
}

static void exceptions(uint32_t st)
{
    uint32_t r = 0;
    /* an encoding of ADDU.QB's group no instruction has: op 0x02 */
    __asm__ volatile(".word 0x7c221090" : : : "memory");
    exc_line("addu.qb group op 2");
    /* Status.MX clear: DSP state disabled */
    __asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(st & ~(1u << 24)));
    __asm__ volatile("addu.qb %0, %0, %0" : "+r"(r));
    exc_line("addu.qb without MX");
    __asm__ volatile("mfhi %0, $ac1" : "=r"(r));
    exc_line("mfhi $ac1 without MX");
    __asm__ volatile("mfhi %0, $ac0" : "=r"(r));
    exc_line("mfhi $ac0 without MX");
    __asm__ volatile("rddsp %0, 0x3f" : "=r"(r));
    exc_line("rddsp without MX");
    __asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(st));
}

int main(void)
{
    vector(0x80000180u, exc_handler);
    uint32_t st;
    __asm__ volatile("mfc0 %0, $12" : "=r"(st));
    st |= 1u << 24;                                 /* MX */
    st &= ~((1u << 22) | (1u << 2) | (1u << 1) | 1u);   /* BEV, ERL, EXL, IE */
    __asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(st));

    addu_qb(); addu_s_qb(); subu_qb(); subu_s_qb();
    addu_ph(); addu_s_ph(); subu_ph(); subu_s_ph();
    addq_ph(); addq_s_ph(); subq_ph(); subq_s_ph();
    addq_s_w(); subq_s_w(); addsc(); addwc(); modsub();
    adduh_qb(); adduh_r_qb(); subuh_qb(); subuh_r_qb();
    addqh_ph(); addqh_r_ph(); subqh_ph(); subqh_r_ph();
    addqh_w(); addqh_r_w(); subqh_w(); subqh_r_w();
    muleu_s_ph_qbl(); muleu_s_ph_qbr(); mulq_rs_ph(); mulq_s_ph();
    muleq_s_w_phl(); muleq_s_w_phr(); mul_ph(); mul_s_ph();
    mulq_s_w(); mulq_rs_w();
    precrq_qb_ph(); precr_qb_ph(); precrq_ph_w(); precrq_rs_ph_w();
    precrqu_s_qb_ph(); packrl_ph(); pick_qb(); pick_ph();
    cmpgu_eq_qb(); cmpgu_lt_qb(); cmpgu_le_qb();
    cmpgdu_eq_qb(); cmpgdu_lt_qb(); cmpgdu_le_qb();
    cmpu_eq_qb(); cmpu_lt_qb(); cmpu_le_qb();
    cmp_eq_ph(); cmp_lt_ph(); cmp_le_ph();
    shllv_qb(); shrlv_qb(); shrav_qb(); shrav_r_qb();
    shllv_ph(); shllv_s_ph(); shrlv_ph(); shrav_ph(); shrav_r_ph();
    shllv_s_w(); shrav_r_w();
    shll_qb(); shrl_qb(); shra_qb(); shra_r_qb();
    shll_ph(); shll_s_ph(); shrl_ph(); shra_ph(); shra_r_ph();
    shll_s_w(); shra_r_w();
    absq_s_qb(); absq_s_ph(); absq_s_w(); raddu_w_qb();
    preceq_w_phl(); preceq_w_phr();
    precequ_ph_qbl(); precequ_ph_qbr(); precequ_ph_qbla(); precequ_ph_qbra();
    preceu_ph_qbl(); preceu_ph_qbr(); preceu_ph_qbla(); preceu_ph_qbra();
    bitrev(); replv_qb(); replv_ph(); repl();
    precr_sra_ph_w_0(); precr_sra_ph_w_1(); precr_sra_ph_w_31();
    precr_sra_r_ph_w_0(); precr_sra_r_ph_w_1(); precr_sra_r_ph_w_16();
    append_0(); append_1(); append_31();
    prepend_0(); prepend_1(); prepend_31();
    balign_0(); balign_1(); balign_2(); balign_3();
    insv();
    dpau_h_qbl(); dpau_h_qbr(); dpsu_h_qbl(); dpsu_h_qbr();
    dpa_w_ph(); dpax_w_ph(); dps_w_ph(); dpsx_w_ph();
    dpaq_s_w_ph(); dpaqx_s_w_ph(); dpsq_s_w_ph(); dpsqx_s_w_ph();
    dpaqx_sa_w_ph(); dpsqx_sa_w_ph(); dpaq_sa_l_w(); dpsq_sa_l_w();
    mulsaq_s_w_ph(); mulsa_w_ph();
    maq_s_w_phl(); maq_s_w_phr(); maq_sa_w_phl(); maq_sa_w_phr();
    madd_ac(); maddu_ac(); msub_ac(); msubu_ac(); mult_ac(); multu_ac();
    extr_w(); extr_r_w(); extr_rs_w(); extr_s_h(); extp(); extpdp();
    extrv_w(); extrv_r_w(); extrv_rs_w(); extrv_s_h(); extpv(); extpdpv();
    shilo_a(); shilo_b(); shilo_c(); shilo_d(); shilov(); mthlip();
    other_acc();
    masks();
    bposge32();
    loads();
    exceptions(st);
    puts_("done\n");
    SOFTRES = 0x42;
    for (;;)
        ;
}

void _start(void)
{
    main();
}
