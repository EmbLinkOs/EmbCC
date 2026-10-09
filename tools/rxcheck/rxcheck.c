/* Hands EmbCC's whole Renesas RX vocabulary to two referees, one
 * instruction at a time.
 *
 *   rxcheck --vocab BASE   one line per form:
 *                              QEMU's disassembly of it | the bytes
 *                              src/arch/rx/emit.c produced | GNU as syntax
 *                          BASE is the address the bytes are loaded at,
 *                          which branch targets are printed against
 *   rxcheck --refuse N     provokes encoder range check N, which must stop
 *                          the process with an internal error;
 *                          `--refuse list` prints how many there are
 *
 * The first column is what the instruction MEANS, written the way QEMU's
 * RX disassembler (target/rx/disas.c) prints it -- its spacing, its
 * `#0x%08x` for an immediate of 256 and up, its byte displacements and
 * absolute branch targets included. tests/golden/rx-encoding.sh loads the
 * bytes into qemu-system-rx and has its monitor disassemble them (`x/Ni`):
 * QEMU's decoder, which is what the emulator EXECUTES, must read back the
 * same instruction from the same number of bytes. The third column is the
 * same instruction in GNU as syntax; where an rx-elf binutils exists the
 * golden assembles it and compares the bytes outright, which also checks
 * that the encoder chose the shortest form, as GNU as does.
 *
 * A line's three columns come from ONE entry -- encoded and formatted in
 * the same block -- so a form printed but not encoded, or the reverse,
 * cannot shift every comparison after it and blame the wrong instruction.
 *
 * The sweep puts registers below and above r7 in every field (the dsp:5
 * forms only reach r0..r7), immediates at both ends of each li width and
 * of #uimm4/#uimm8, and displacements at the ends of each scaled field.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/rx/emit.h"

static struct code C;
static unsigned long g_base;
static char q[200], g[200];
static int g_nlines;

#define QF(...) (void)snprintf(q, sizeof q, __VA_ARGS__)
#define GF(...) (void)snprintf(g, sizeof g, __VA_ARGS__)

static void line(int at)
{
    if (C.len - at < 1 || C.len - at > 8) {
        fprintf(stderr, "rxcheck: an entry emitted %d bytes (%s)\n",
                C.len - at, q);
        exit(2);
    }
    printf("%s|", q);
    for (int k = at; k < C.len; k++)
        printf("%02x", C.p[k]);
    printf("|%s\n", g);
    g_nlines++;
}

/* QEMU's prt_ir: a small immediate in decimal, else eight hex digits. */
static const char *qimm(long v)
{
    static char b[4][32];
    static int k;
    int iv = (int)(unsigned int)(unsigned long)v;
    k = (k + 1) & 3;
    if (iv < 0x100)
        snprintf(b[k], sizeof b[k], "#%d", iv);
    else
        snprintf(b[k], sizeof b[k], "#0x%08x", (unsigned)iv);
    return b[k];
}

static const int R[] = { 0, 1, 2, 5, 7, 8, 10, 14, 15 };
#define NR ((int)(sizeof R / sizeof R[0]))
static const long IMM[] = {
    0, 1, 7, 15, 16, 100, 127, 128, 200, 255, 256, 1000, 32767, 32768,
    65535, 65536, 0x7fffff, 0x800000, 0x12345678, 0x7fffffff,
    -1, -2, -16, -100, -128, -129, -32768, -32769, -8388608, -8388609,
    -2147483647L - 1
};
#define NIMM ((int)(sizeof IMM / sizeof IMM[0]))
static const char SZ[] = { 'b', 'w', 'l' };

static void v_rr(void)
{
    static const int ops[] = {
        RX_MOV, RX_ADD, RX_SUB, RX_CMP, RX_AND, RX_OR, RX_XOR, RX_TST,
        RX_MUL, RX_ADC, RX_SBB, RX_DIV, RX_DIVU, RX_EMUL, RX_EMULU, RX_MAX,
        RX_MIN, RX_NEG, RX_NOT, RX_ABS, RX_SHLL, RX_SHLR, RX_SHAR, RX_ROTL,
        RX_ROTR, RX_REVL, RX_REVW, RX_XCHG
    };
    for (unsigned o = 0; o < sizeof ops / sizeof ops[0]; o++)
        for (int a = 0; a < NR; a++)
            for (int b = 0; b < NR; b++) {
                int op = ops[o], rs = R[a], rd = R[b], at = C.len;
                const char *n = rx_op_name(op);
                if ((op == RX_EMUL || op == RX_EMULU) && rd == 15)
                    continue;
                rx_rr(&C, op, rs, rd);
                if (op == RX_MOV)
                    QF("mov.l\tr%d, r%d", rs, rd);
                else if ((op == RX_NEG || op == RX_NOT || op == RX_ABS) &&
                         rs == rd)
                    QF("%s\tr%d", n, rd);
                else
                    QF("%s\tr%d, r%d", n, rs, rd);
                if (op == RX_MOV)
                    GF("mov.l\tr%d, r%d", rs, rd);
                else
                    GF("%s\tr%d, r%d", n, rs, rd);
                /* gas writes neg/not/abs rX, rX in the two-byte form */
                if ((op == RX_NEG || op == RX_NOT || op == RX_ABS) &&
                    rs == rd)
                    GF("-");
                line(at);
            }
    for (int b = 0; b < NR; b++) {
        static const int one[] = { RX_NEG, RX_NOT, RX_ABS };
        for (int o = 0; o < 3; o++) {
            int at = C.len;
            rx_r(&C, one[o], R[b]);
            QF("%s\tr%d", rx_op_name(one[o]), R[b]);
            GF("%s\tr%d", rx_op_name(one[o]), R[b]);
            line(at);
        }
    }
}

static void v_ri(void)
{
    static const int ops[] = {
        RX_MOV, RX_ADD, RX_SUB, RX_CMP, RX_AND, RX_OR, RX_XOR, RX_TST,
        RX_MUL, RX_ADC, RX_DIV, RX_DIVU, RX_EMUL, RX_EMULU, RX_MAX, RX_MIN,
        RX_STZ, RX_STNZ
    };
    for (unsigned o = 0; o < sizeof ops / sizeof ops[0]; o++)
        for (int k = 0; k < NIMM; k++)
            for (int b = 0; b < NR; b += 3) {
                int op = ops[o], rd = R[b], at = C.len;
                long v = IMM[k];
                int iv = (int)(unsigned int)(unsigned long)v;
                int len;
                if ((op == RX_EMUL || op == RX_EMULU) && rd == 15)
                    continue;
                rx_ri(&C, op, v, rd);
                len = C.len - at;
                if (len != rx_ri_len(op, v)) {
                    fprintf(stderr, "rxcheck: rx_ri_len(%s, %ld) says %d, "
                            "rx_ri wrote %d\n", rx_op_name(op), v,
                            rx_ri_len(op, v), len);
                    exit(2);
                }
                if (op == RX_ADD || (op == RX_SUB && !(iv >= 0 && iv <= 15))) {
                    /* QEMU's ADD_irr: a 4-bit unsigned or a negative
                     * immediate as `add #n, rd`, anything else in hex
                     * with the source register spelled out */
                    int a = op == RX_SUB ? (int)(0u - (unsigned)iv) : iv;
                    if (a < 0x10)
                        QF("add\t#%d, r%d", a, rd);
                    else
                        QF("add\t#0x%08x, r%d, r%d", (unsigned)a, rd, rd);
                    GF("add\t#%d, r%d", a, rd);
                } else if (op == RX_SUB) {
                    QF("sub\t#%d, r%d", iv, rd);
                    GF("sub\t#%d, r%d", iv, rd);
                } else if (op == RX_MOV) {
                    QF("mov.l\t%s, r%d", qimm(v), rd);
                    GF("mov.l\t#%d, r%d", iv, rd);
                } else {
                    QF("%s\t%s, r%d", rx_op_name(op), qimm(v), rd);
                    GF("%s\t#%d, r%d", rx_op_name(op), iv, rd);
                }
                line(at);
            }
    /* the fixed six-byte form */
    for (int b = 0; b < NR; b++) {
        int at = C.len, f = rx_mov_abs(&C, R[b], 0x1000u + (unsigned)b);
        if (f != at + 2) { fprintf(stderr, "rxcheck: mov_abs field\n"); exit(2); }
        QF("mov.l\t%s, r%d", qimm(0x1000 + b), R[b]);
        GF("-");
        line(at);
    }
}

static void v_three(void)
{
    static const int ops[] = { RX_ADD, RX_SUB, RX_MUL, RX_AND, RX_OR };
    for (unsigned o = 0; o < sizeof ops / sizeof ops[0]; o++)
        for (int a = 0; a < NR; a += 2)
            for (int b = 1; b < NR; b += 2)
                for (int d = 0; d < NR; d += 3) {
                    int op = ops[o], rs = R[a], rs2 = R[b], rd = R[d];
                    int at = C.len;
                    const char *n = rx_op_name(op);
                    rx_rrr(&C, op, rs, rs2, rd);
                    if (op == RX_AND)
                        QF("and\tr%d,r%d, r%d", rs, rs2, rd);
                    else if (op == RX_MUL)
                        QF("mul\tr%d,r%d,r%d", rs, rs2, rd);
                    else
                        QF("%s\tr%d, r%d, r%d", n, rs, rs2, rd);
                    GF("%s\tr%d, r%d, r%d", n, rs, rs2, rd);
                    line(at);
                }
    for (int k = 0; k < NIMM; k++)
        for (int a = 0; a < NR; a += 2)
            for (int d = 1; d < NR; d += 3) {
                long v = IMM[k];
                int iv = (int)(unsigned int)(unsigned long)v;
                int rs = R[a], rd = R[d], at = C.len;
                if (rs == rd)
                    continue;
                rx_add3(&C, v, rs, rd);
                if (C.len - at != rx_add3_len(v, rs, rd)) {
                    fprintf(stderr, "rxcheck: rx_add3_len\n");
                    exit(2);
                }
                QF("add\t#0x%08x, r%d, r%d", (unsigned)iv, rs, rd);
                GF("add\t#%d, r%d, r%d", iv, rs, rd);
                line(at);
            }
}

static void v_shift(void)
{
    static const int ops[] = { RX_SHLL, RX_SHLR, RX_SHAR };
    static const int N[] = { 0, 1, 7, 15, 16, 24, 31 };
    for (int o = 0; o < 3; o++)
        for (int k = 0; k < 7; k++)
            for (int a = 0; a < NR; a += 2)
                for (int d = 0; d < NR; d += 3) {
                    int op = ops[o], rs = R[a], rd = R[d], n = N[k];
                    int at = C.len;
                    rx_shift_i(&C, op, n, rs, rd);
                    if (rs == rd) {
                        QF("%s\t#%d, r%d", rx_op_name(op), n, rd);
                        GF("%s\t#%d, r%d", rx_op_name(op), n, rd);
                    } else {
                        QF("%s\t#%d, r%d, r%d", rx_op_name(op), n, rs, rd);
                        GF("%s\t#%d, r%d, r%d", rx_op_name(op), n, rs, rd);
                    }
                    line(at);
                }
    for (int o = 0; o < 2; o++)
        for (int k = 0; k < 7; k++)
            for (int d = 0; d < NR; d++) {
                int op = o ? RX_ROTR : RX_ROTL, n = N[k], rd = R[d];
                int at = C.len;
                rx_shift_i(&C, op, n, rd, rd);
                QF("%s\t#%d, r%d", rx_op_name(op), n, rd);
                GF("%s\t#%d, r%d", rx_op_name(op), n, rd);
                line(at);
            }
}

static void v_ext(void)
{
    for (int sz = RX_B; sz <= RX_W; sz++)
        for (int sign = 0; sign < 2; sign++)
            for (int a = 0; a < NR; a++)
                for (int d = 0; d < NR; d += 2) {
                    int at = C.len;
                    rx_ext(&C, sz, sign, R[a], R[d]);
                    QF("%s.%c\tr%d, r%d", sign ? "mov" : "movu", SZ[sz],
                       R[a], R[d]);
                    GF("%s.%c\tr%d, r%d", sign ? "mov" : "movu", SZ[sz],
                       R[a], R[d]);
                    line(at);
                }
}

/* Displacements, in units of the access size. */
static const long DU[] = { 0, 1, 3, 7, 31, 32, 100, 255, 256, 4097, 65535 };
#define NDU ((int)(sizeof DU / sizeof DU[0]))

static void v_mem(void)
{
    for (int sz = RX_B; sz <= RX_L; sz++) {
        int scale = sz == RX_B ? 1 : sz == RX_W ? 2 : 4;
        for (int k = 0; k < NDU; k++)
            for (int a = 0; a < NR; a += 2)
                for (int d = 1; d < NR; d += 2)
                    for (int sign = 0; sign < 2; sign++) {
                        long dsp = DU[k] * scale;
                        int rs = R[a], rd = R[d], at = C.len;
                        int short5 = DU[k] >= 1 && DU[k] <= 31 && rs < 8 && rd < 8;
                        const char *mn = sign || sz == RX_L ? "mov" : "movu";
                        if (sz == RX_L && !sign)
                            continue;
                        if (!rx_load_ok(sz, sign, dsp))
                            continue;       /* refusal 30 */
                        rx_load(&C, sz, sign, dsp, rs, rd);
                        if (dsp)
                            QF("%s.%c\t%ld[r%d], r%d", mn, SZ[sz], dsp, rs, rd);
                        else
                            QF("%s.%c\t[r%d], r%d", mn, SZ[sz], rs, rd);
                        (void)short5;
                        if (dsp)
                            GF("%s.%c\t%ld[r%d], r%d", mn, SZ[sz], dsp, rs, rd);
                        else
                            GF("%s.%c\t[r%d], r%d", mn, SZ[sz], rs, rd);
                        line(at);
                    }
        for (int k = 0; k < NDU; k++)
            for (int a = 0; a < NR; a += 2)
                for (int d = 1; d < NR; d += 2) {
                    long dsp = DU[k] * scale;
                    int rs = R[a], rd = R[d], at = C.len;
                    int short5 = DU[k] >= 1 && DU[k] <= 31 && rs < 8 && rd < 8;
                    rx_store(&C, sz, rs, dsp, rd);
                    /* QEMU spells the dsp:5 form without the space -- and
                     * prints the long form's two registers SWAPPED: its
                     * translator stores the low nibble's register through
                     * the high nibble's, as GNU as encodes `mov rs, [rd]`
                     * (rd high), but disas.c names the high one first. The
                     * text below is QEMU's; GNU as's bytes and the board
                     * referee the meaning. */
                    if (short5)
                        QF("mov.%c\tr%d,%ld[r%d]", SZ[sz], rs, dsp, rd);
                    else
                        QF("mov.%c\tr%d, %ld[r%d]", SZ[sz], rd, dsp, rs);
                    if (!dsp)
                        QF(short5 ? "mov.%c\tr%d,[r%d]" : "mov.%c\tr%d, [r%d]",
                           SZ[sz], short5 ? rs : rd, short5 ? rd : rs);
                    if (dsp)
                        GF("mov.%c\tr%d, %ld[r%d]", SZ[sz], rs, dsp, rd);
                    else
                        GF("mov.%c\tr%d, [r%d]", SZ[sz], rs, rd);
                    line(at);
                }
        /* immediates to memory */
        {
            static const long SI[] = { 0, 1, 100, 127, 128, 255, 256, -1,
                                       -128, -129, 32767, -32768, 65535,
                                       0x123456, -2147483647L - 1 };
            for (unsigned k = 0; k < sizeof SI / sizeof SI[0]; k++)
                for (int j = 0; j < NDU; j += 2)
                    for (int d = 0; d < NR; d += 4) {
                        long dsp = DU[j] * scale, v = SI[k];
                        int rd = R[d], at = C.len, shown;
                        if (sz == RX_B) v = (signed char)(v & 0xff);
                        if (sz == RX_W) v = (short)(v & 0xffff);
                        if (!rx_store_imm_ok(sz, dsp))
                            continue;       /* refusal 31 */
                        rx_store_imm(&C, sz, v, dsp, rd);
                        /* the dsp:5 form holds an UNSIGNED byte */
                        shown = (int)v;
                        if (C.p[at] >= 0x3c && C.p[at] <= 0x3e)
                            shown = (int)(v & 0xff);
                        if (dsp)
                            QF("mov.%c\t#%d,%ld[r%d]", SZ[sz], shown, dsp, rd);
                        else
                            QF("mov.%c\t#%d,[r%d]", SZ[sz], shown, rd);
                        /* (a byte written unsigned, so GNU as also takes
                         * the #uimm8 form for -128..-1) */
                        if (dsp)
                            GF("mov.%c\t#%d, %ld[r%d]", SZ[sz], shown, dsp, rd);
                        else
                            GF("mov.%c\t#%d, [r%d]", SZ[sz], shown, rd);
                        line(at);
                    }
        }
        /* indexed */
        for (int a = 0; a < NR; a += 2)
            for (int b = 1; b < NR; b += 2)
                for (int d = 0; d < NR; d += 4)
                    for (int sign = 0; sign < 2; sign++) {
                        int ri = R[a], rb = R[b], rd = R[d], at = C.len;
                        const char *mn = sign || sz == RX_L ? "mov" : "movu";
                        if (sz == RX_L && !sign)
                            continue;
                        rx_load_idx(&C, sz, sign, ri, rb, rd);
                        /* QEMU names movu's indexed form `mov` */
                        QF("mov.%c\t[r%d,r%d], r%d", SZ[sz], ri, rb, rd);
                        GF("%s.%c\t[r%d, r%d], r%d", mn, SZ[sz], ri, rb, rd);
                        line(at);
                        if (sign) {
                            at = C.len;
                            rx_store_idx(&C, sz, rd, ri, rb);
                            QF("mov.%c\tr%d, [r%d, r%d]", SZ[sz], rd, ri, rb);
                            GF("mov.%c\tr%d, [r%d, r%d]", SZ[sz], rd, ri, rb);
                            line(at);
                        }
                    }
    }
}

static void v_memex(void)
{
    static const int ops[] = {
        RX_ADD, RX_SUB, RX_CMP, RX_AND, RX_OR, RX_MUL, RX_XOR, RX_TST,
        RX_MAX, RX_MIN, RX_DIV, RX_DIVU, RX_EMUL, RX_EMULU, RX_XCHG
    };
    static const struct { int size, sign; const char *s; int scale; } M[] = {
        { RX_B, 0, ".ub", 1 }, { RX_B, 1, ".b", 1 }, { RX_W, 1, ".w", 2 },
        { RX_W, 0, ".uw", 2 }, { RX_L, 1, ".l", 4 }
    };
    static const long D[] = { 0, 1, 255, 256, 65535 };
    for (unsigned o = 0; o < sizeof ops / sizeof ops[0]; o++)
        for (int m = 0; m < 5; m++)
            for (int k = 0; k < 5; k++) {
                int op = ops[o], rs = R[(o + m) % NR], rd = R[(o + k) % NR];
                long dsp = D[k] * M[m].scale;
                int at = C.len;
                if ((op == RX_EMUL || op == RX_EMULU) && rd == 15)
                    rd = 14;
                if (!rx_rm_ok(op, M[m].size, M[m].sign, dsp)) {
                    fprintf(stderr, "rxcheck: rx_rm_ok refuses a valid form\n");
                    exit(2);
                }
                rx_rm(&C, op, M[m].size, M[m].sign, dsp, rs, rd);
                if (dsp)
                    QF("%s\t%ld[r%d]%s, r%d", rx_op_name(op), dsp, rs, M[m].s, rd);
                else
                    QF("%s\t[r%d]%s, r%d", rx_op_name(op), rs, M[m].s, rd);
                if (dsp)
                    GF("%s\t%ld[r%d]%s, r%d", rx_op_name(op), dsp, rs, M[m].s, rd);
                else
                    GF("%s\t[r%d]%s, r%d", rx_op_name(op), rs, M[m].s, rd);
                line(at);
            }
}

static void v_adcsbb(void)
{
    static const long D[] = { 0, 4, 1020, 1024, 262140 };
    for (int o = 0; o < 2; o++)
        for (int k = 0; k < 5; k++)
            for (int a = 0; a < NR; a += 3) {
                int op = o ? RX_SBB : RX_ADC, rs = R[a], rd = R[(a + k) % NR];
                int at = C.len;
                if (!rx_rm_ok(op, RX_L, 1, D[k])) {
                    fprintf(stderr, "rxcheck: rx_rm_ok refuses adc/sbb\n");
                    exit(2);
                }
                rx_rm(&C, op, RX_L, 1, D[k], rs, rd);
                if (D[k])
                    QF("%s\t%ld[r%d], r%d", rx_op_name(op), D[k], rs, rd);
                else
                    QF("%s\t[r%d], r%d", rx_op_name(op), rs, rd);
                if (op == RX_SBB && D[k])  /* QEMU's prt_ldmi adds .l */
                    QF("sbb\t%ld[r%d].l, r%d", D[k], rs, rd);
                else if (op == RX_SBB)
                    QF("sbb\t[r%d].l, r%d", rs, rd);
                if (D[k])
                    GF("%s\t%ld[r%d].l, r%d", rx_op_name(op), D[k], rs, rd);
                else
                    GF("%s\t[r%d].l, r%d", rx_op_name(op), rs, rd);
                line(at);
            }
}

static void v_stack(void)
{
    for (int a = 0; a < NR; a++) {
        int at = C.len;
        rx_push(&C, R[a]);
        QF("push\tr%d", R[a]); GF("push.l\tr%d", R[a]); line(at);
        at = C.len;
        rx_pop(&C, R[a]);
        QF("pop\tr%d", R[a]); GF("pop\tr%d", R[a]); line(at);
    }
    for (int a = 1; a <= 14; a += 3)
        for (int b = a + 1; b <= 15; b += 4) {
            int at = C.len;
            rx_pushm(&C, a, b);
            QF("pushm\tr%d-r%d", a, b); GF("pushm\tr%d-r%d", a, b); line(at);
            at = C.len;
            rx_popm(&C, a, b);
            QF("popm\tr%d-r%d", a, b); GF("popm\tr%d-r%d", a, b); line(at);
        }
    {
        int at = C.len;
        rx_rts(&C);
        QF("rts"); GF("rts"); line(at);
    }
    for (long n = 0; n <= 1020; n += n < 16 ? 4 : 252) {
        int at = C.len;
        rx_rtsd(&C, n);
        QF("rtsd\t#%ld", n); GF("rtsd\t#%ld", n); line(at);
    }
    for (int a = 1; a <= 15; a += 2)
        for (int b = a; b <= 15; b += 5) {
            long n = 4L * (b - a + 1) + 8;
            int at = C.len;
            rx_rtsd_m(&C, n, a, b);
            QF("rtsd\t#%ld, r%d - r%d", n, a, b);
            GF("rtsd\t#%ld, r%d-r%d", n, a, b);
            line(at);
        }
}

static void v_branch(void)
{
    static const long DS[] = { 3, 4, 7, 8, 9, 10 };
    static const long DB[] = { -128, -1, 0, 2, 11, 127 };
    static const long DW[] = { -32768, -129, 128, 1000, 32767 };
    static const long DA[] = { -8388608L, -32769, 32768, 8388607L };
    static const char K[] = { 's', 'b', 'w', 'a' };
    for (int kind = RX_BR_S; kind <= RX_BR_A; kind++)
        for (int cond = 0; cond <= RX_ALWAYS; cond++) {
            const long *ds = kind == RX_BR_S ? DS : kind == RX_BR_B ? DB
                           : kind == RX_BR_W ? DW : DA;
            int nd = kind == RX_BR_S ? 6 : kind == RX_BR_B ? 6
                   : kind == RX_BR_W ? 5 : 4;
            if (!rx_branch_ok(kind, cond))
                continue;
            for (int k = 0; k < nd; k++) {
                int at = C.len;
                unsigned long target = g_base + (unsigned long)at +
                                       (unsigned long)ds[k];
                rx_branch_d(&C, kind, cond, ds[k]);
                if (C.len - at != rx_branch_len(kind)) {
                    fprintf(stderr, "rxcheck: rx_branch_len\n");
                    exit(2);
                }
                QF("b%s.%c\t%08lx", rx_cond_name(cond), K[kind],
                   target & 0xffffffffUL);
                GF("-");
                line(at);
            }
        }
    /* every placeholder, patched from far and near */
    for (int kind = RX_BR_S; kind <= RX_BR_A; kind++)
        for (int cond = 0; cond <= RX_ALWAYS; cond++) {
            int at;
            long d = kind == RX_BR_S ? 5 : kind == RX_BR_B ? -100
                   : kind == RX_BR_W ? 30000 : -4000000L;
            if (!rx_branch_ok(kind, cond))
                continue;
            at = rx_branch(&C, kind, cond);
            if (!rx_patch_branch(&C, at, (int)(at + d)) ||
                rx_patch_branch(&C, at, (int)(at + (kind == RX_BR_A
                                                    ? 9000000L : 70000L))) ||
                !rx_patch_branch(&C, at, (int)(at + d))) {
                fprintf(stderr, "rxcheck: rx_patch_branch\n");
                exit(2);
            }
            QF("b%s.%c\t%08lx", rx_cond_name(cond), K[kind],
               (g_base + (unsigned long)at + (unsigned long)d) & 0xffffffffUL);
            GF("-");
            line(at);
        }
    {
        int at = C.len;
        rx_bsr_w_d(&C, -32768);
        QF("bsr.w\t%08lx", (g_base + (unsigned long)at - 32768) & 0xffffffffUL);
        GF("-");
        line(at);
        at = C.len;
        rx_bsr_a_d(&C, 8388607L);
        QF("bsr.a\t%08lx", (g_base + (unsigned long)at + 8388607L) & 0xffffffffUL);
        GF("-");
        line(at);
        at = rx_bsr_a(&C);
        QF("bsr.a\t%08lx", (g_base + (unsigned long)at) & 0xffffffffUL);
        GF("-");
        line(at);
        at = rx_bra_a(&C);
        QF("bra.a\t%08lx", (g_base + (unsigned long)at) & 0xffffffffUL);
        GF("-");
        line(at);
    }
    for (int a = 0; a < NR; a++) {
        int at = C.len;
        rx_jmp(&C, R[a]); QF("jmp\tr%d", R[a]); GF("jmp\tr%d", R[a]); line(at);
        at = C.len;
        rx_jsr(&C, R[a]); QF("jsr\tr%d", R[a]); GF("jsr\tr%d", R[a]); line(at);
        at = C.len;
        rx_bra_l(&C, R[a]); QF("bra.l\tr%d", R[a]); GF("bra.l\tr%d", R[a]); line(at);
        at = C.len;
        rx_bsr_l(&C, R[a]); QF("bsr.l\tr%d", R[a]); GF("bsr.l\tr%d", R[a]); line(at);
    }
}

static void v_misc(void)
{
    static const char psw[] = "czso????iu";
    static const char *const cr[] = {
        "psw", "pc", "usp", "fpsw", "", "", "", "", "bpsw", "bpc", "isp",
        "fintv", "intb"
    };
    int at;
    for (int cond = 0; cond <= RX_NO; cond++)
        for (int d = 0; d < NR; d += 2) {
            at = C.len;
            rx_scc(&C, cond, R[d]);
            QF("sc%s.l\tr%d", rx_cond_name(cond), R[d]);
            GF("sc%s.l\tr%d", rx_cond_name(cond), R[d]);
            line(at);
        }
    for (int d = 0; d < NR; d += 2) {
        /* (QEMU's disassembler names rolc `rorc` too) */
        at = C.len; rx_rolc(&C, R[d]); QF("rorc\tr%d", R[d]); GF("rolc\tr%d", R[d]); line(at);
        at = C.len; rx_rorc(&C, R[d]); QF("rorc\tr%d", R[d]); GF("rorc\tr%d", R[d]); line(at);
    }
    at = C.len; rx_smovf(&C); QF("smovf"); GF("smovf"); line(at);
    at = C.len; rx_sstr_b(&C); QF("sstr.b"); GF("sstr.b"); line(at);
    for (int k = 0; k <= 12; k++) {
        if ((k > 3 && k < 8) || k == 1)
            continue;
        at = C.len; rx_pushc(&C, k); QF("push\t%s", cr[k]); GF("pushc\t%s", cr[k]); line(at);
        /* QEMU prints popc with a stray `r` before the name */
        at = C.len; rx_popc(&C, k); QF("pop\tr%s", cr[k]); GF("popc\t%s", cr[k]); line(at);
    }
    at = C.len; rx_nop(&C);  QF("nop");  GF("nop");  line(at);
    at = C.len; rx_brk(&C);  QF("brk");  GF("brk");  line(at);
    at = C.len; rx_wait(&C); QF("wait"); GF("wait"); line(at);
    at = C.len; rx_rte(&C);  QF("rte");  GF("rte");  line(at);
    for (int n = 0; n < 256; n += 85) {
        at = C.len; rx_int(&C, n); QF("int\t#%d", n); GF("int\t#%d", n); line(at);
    }
    for (int b = 0; b <= 9; b++) {
        if (b > 3 && b < 8)
            continue;
        at = C.len; rx_setpsw(&C, b);
        QF("setpsw\t%c", psw[b]); GF("setpsw\t%c", psw[b]); line(at);
        at = C.len; rx_clrpsw(&C, b);
        QF("clrpsw\t%c", psw[b]); GF("clrpsw\t%c", psw[b]); line(at);
    }
    for (int k = 0; k <= 12; k++) {
        if (k > 3 && k < 8)
            continue;
        for (int a = 0; a < NR; a += 4) {
            if (k != 1) {
                at = C.len; rx_mvtc(&C, R[a], k);
                QF("mvtc\tr%d, %s", R[a], cr[k]);
                GF("mvtc\tr%d, %s", R[a], cr[k]);
                line(at);
            }
            at = C.len; rx_mvfc(&C, k, R[a]);
            QF("mvfc\t%s, r%d", cr[k], R[a]);
            GF("mvfc\t%s, r%d", cr[k], R[a]);
            line(at);
        }
        if (k != 1)
            for (int j = 0; j < NIMM; j += 5) {
                int iv = (int)(unsigned int)(unsigned long)IMM[j];
                at = C.len; rx_mvtc_i(&C, IMM[j], k);
                QF("mvtc\t#0x%08x, %s", (unsigned)iv, cr[k]);
                GF("mvtc\t#%d, %s", iv, cr[k]);
                line(at);
            }
    }
    for (int n = 0; n < 16; n += 5) {
        at = C.len; rx_mvtipl(&C, n);
        QF("movtipl\t#%d", n); GF("mvtipl\t#%d", n); line(at);
    }
    {
        static const int bits[] = { 0, 1, 15, 16, 31 };
        static const char *const nm[] = { "bset", "bclr", "btst", "bnot" };
        for (int o = RX_BSET; o <= RX_BNOT; o++)
            for (int k = 0; k < 5; k++)
                for (int d = 0; d < NR; d += 2) {
                    at = C.len;
                    rx_bit_i(&C, o, bits[k], R[d]);
                    if (o == RX_BCLR)
                        QF("bclr\t#%d,r%d", bits[k], R[d]);
                    else
                        QF("%s\t#%d, r%d", nm[o], bits[k], R[d]);
                    GF("%s\t#%d, r%d", nm[o], bits[k], R[d]);
                    line(at);
                }
        for (int cond = 0; cond <= RX_NO; cond++)
            for (int k = 0; k < 5; k++) {
                int d = R[(cond + k) % NR];
                at = C.len;
                rx_bmcnd(&C, cond, bits[k], d);
                QF("bm%s\t#%d, r%d", rx_cond_name(cond), bits[k], d);
                GF("bm%s\t#%d, r%d", rx_cond_name(cond), bits[k], d);
                line(at);
            }
    }
}

/* ---- the range checks -------------------------------------------------- */

static void refuse(int n)
{
    switch (n) {
    case 0:  rx_rr(&C, RX_ADD, 16, 1); break;          /* no r16 */
    case 1:  rx_rr(&C, RX_EMUL, 1, 15); break;         /* r15:r16 */
    case 2:  rx_load(&C, RX_L, 1, -4, 1, 2); break;     /* negative dsp */
    case 3:  rx_load(&C, RX_L, 1, 6, 1, 2); break;      /* not a multiple */
    case 4:  rx_load(&C, RX_W, 1, 2L * 65536, 1, 2); break; /* too far */
    case 5:  rx_store(&C, RX_B, 1, 65536, 2); break;
    case 6:  rx_shift_i(&C, RX_SHLL, 32, 1, 1); break;
    case 7:  rx_shift_i(&C, RX_ROTL, 3, 1, 2); break;   /* no 3-op rotate */
    case 8:  rx_pushm(&C, 0, 3); break;                 /* not r0 */
    case 9:  rx_pushm(&C, 3, 3); break;                 /* push, not pushm */
    case 10: rx_rtsd(&C, 1024); break;
    case 11: rx_rtsd(&C, 6); break;
    case 12: rx_rtsd_m(&C, 4, 6, 7); break;             /* pops more */
    case 13: rx_branch_d(&C, RX_BR_B, RX_LT, 128); break;
    case 14: rx_branch_d(&C, RX_BR_W, RX_LT, 0); break; /* no blt.w */
    case 15: rx_branch_d(&C, RX_BR_S, RX_EQ, 2); break;
    case 16: rx_branch_d(&C, RX_BR_A, RX_ALWAYS, 8388608L); break;
    case 17: rx_scc(&C, RX_ALWAYS, 1); break;
    case 18: rx_mvtc(&C, 1, 1); break;                  /* pc */
    case 19: rx_setpsw(&C, 5); break;
    case 20: rx_rm(&C, RX_ADC, RX_W, 1, 0, 1, 2); break;
    case 21: rx_rm(&C, RX_ADD, RX_W, 1, 3, 1, 2); break;
    case 22: rx_ri(&C, RX_SBB, 1, 2); break;
    case 23: rx_store_imm(&C, RX_L, 1, 2, 3); break;
    case 24: rx_bit_i(&C, RX_BSET, 32, 1); break;
    case 25: rx_int(&C, 256); break;
    case 26: rx_load_idx(&C, 3, 1, 1, 2, 3); break;
    case 27: rx_rrr(&C, RX_XOR, 1, 2, 3); break;        /* no 3-op xor */
    case 28: rx_ext(&C, RX_L, 1, 1, 2); break;
    case 29: rx_mvtipl(&C, 16); break;
    case 30: rx_load(&C, RX_B, 0, 32768, 1, 2); break;  /* QEMU: signed */
    case 31: rx_store_imm(&C, RX_W, 1, 65536, 3); break;
    default: break;
    }
}
#define NREFUSE 32

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--refuse")) {
        if (argc >= 3 && !strcmp(argv[2], "list")) {
            printf("%d\n", NREFUSE);
            return 0;
        }
        refuse(argc >= 3 ? atoi(argv[2]) : -1);
        printf("refusal %s did not fire\n", argc >= 3 ? argv[2] : "?");
        return 0;
    }
    if (argc < 3 || strcmp(argv[1], "--vocab")) {
        fprintf(stderr, "usage: rxcheck --vocab BASE | --refuse N|list\n");
        return 2;
    }
    g_base = strtoul(argv[2], NULL, 0);
    v_rr();
    v_ri();
    v_three();
    v_shift();
    v_ext();
    v_mem();
    v_memex();
    v_adcsbb();
    v_stack();
    v_branch();
    v_misc();
    return 0;
}
