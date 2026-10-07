/* Hands EmbCC's whole SPARC V8 vocabulary to llvm-mc and compares the
 * encodings, one instruction at a time.
 *
 *   sparccheck --vocab     one line per form: the assembly, a '|', and the
 *                          word src/arch/sparc/emit.c produced for it (its
 *                          bytes in memory order, which on SPARC is always
 *                          big-endian)
 *   sparccheck --li        executes every sparc_li sequence in a small
 *                          interpreter and checks the value and the length
 *                          sparc_li_len promised
 *   sparccheck --refuse N  provokes encoder range check N, which must stop
 *                          the process with an internal error;
 *                          `--refuse list` prints how many there are
 *
 * The text and the word of a line come from ONE call -- each entry formats
 * its assembly and encodes through emit.c in the same macro -- so a form
 * printed but not encoded cannot shift every comparison after it.
 *
 * tests/golden/sparc-encoding.sh has llvm-mc (-triple=sparc -mcpu=leon3)
 * assemble the text into an object and compares its .text word by word.
 * Branches and calls are written `.+N`, which llvm-mc resolves within the
 * section, so their displacement fields are compared like everything
 * else.
 *
 * The sweep puts every register class in every field and the immediates
 * at both ends of the signed 13-bit field: a SPARC mistake is a valid
 * different instruction -- rs1 and rd exchanged, a branch displacement
 * counted from the delay slot, an op3 one off (addx for add) -- that a
 * referee sees and a test program may not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/sparc/emit.h"

static struct code C;

#define V(call, ...) do {                                                   \
        int at_ = C.len;                                                    \
        char txt_[160];                                                     \
        call;                                                               \
        if (C.len - at_ != 4) {                                             \
            fprintf(stderr, "sparccheck: an entry emitted %d bytes\n",      \
                    C.len - at_);                                           \
            exit(2);                                                        \
        }                                                                   \
        snprintf(txt_, sizeof txt_, __VA_ARGS__);                           \
        printf("%s|%02x%02x%02x%02x\n", txt_, C.p[at_], C.p[at_ + 1],       \
               C.p[at_ + 2], C.p[at_ + 3]);                                 \
    } while (0)

static const char *rn(int r)
{
    static char b[8][8];
    static int k;
    k = (k + 1) & 7;
    snprintf(b[k], sizeof b[k], "%%%s", sparc_reg_name(r));
    return b[k];
}

static const int R[] = { 0, 1, 2, 4, 5, 7, 8, 9, 13, 14, 15, 16, 19, 23,
                         24, 27, 29, 30, 31 };
#define NR ((int)(sizeof R / sizeof R[0]))
static const long long SIMM[] = { -4096, -4095, -1024, -2, -1, 0, 1, 2,
                                  511, 1023, 2048, 4094, 4095 };
#define NSIMM ((int)(sizeof SIMM / sizeof SIMM[0]))

/* "[%g1+-4]" as llvm-mc spells an address */
static const char *mem(int base, long long off)
{
    static char b[4][40];
    static int k;
    k = (k + 1) & 3;
    snprintf(b[k], sizeof b[k], "[%s+%lld]", rn(base), off);
    return b[k];
}

/* "%g1+-4", the bare address jmpl, rett and flush take */
static const char *addr(int base, long long off)
{
    static char b[4][40];
    static int k;
    k = (k + 1) & 3;
    snprintf(b[k], sizeof b[k], "%s+%lld", rn(base), off);
    return b[k];
}

static void vocab(void)
{
    static const struct { int op; const char *nm; } alu[] = {
        { SP_ADD, "add" }, { SP_AND, "and" }, { SP_OR, "or" },
        { SP_XOR, "xor" }, { SP_SUB, "sub" }, { SP_ANDN, "andn" },
        { SP_ORN, "orn" }, { SP_XNOR, "xnor" }, { SP_ADDX, "addx" },
        { SP_UMUL, "umul" }, { SP_SMUL, "smul" }, { SP_SUBX, "subx" },
        { SP_UDIV, "udiv" }, { SP_SDIV, "sdiv" }, { SP_ADDCC, "addcc" },
        { SP_ANDCC, "andcc" }, { SP_ORCC, "orcc" }, { SP_XORCC, "xorcc" },
        { SP_SUBCC, "subcc" }, { SP_ANDNCC, "andncc" }, { SP_ORNCC, "orncc" },
        { SP_XNORCC, "xnorcc" }, { SP_ADDXCC, "addxcc" },
        { SP_UMULCC, "umulcc" }, { SP_SMULCC, "smulcc" },
        { SP_SUBXCC, "subxcc" }, { SP_UDIVCC, "udivcc" },
        { SP_SDIVCC, "sdivcc" }, { SP_SLL, "sll" }, { SP_SRL, "srl" },
        { SP_SRA, "sra" }
    };
    static const char *const cnm[16] = {
        "bn", "be", "ble", "bl", "bleu", "bcs", "bneg", "bvs",
        "ba", "bne", "bg", "bge", "bgu", "bcc", "bpos", "bvc"
    };
    static const char *const tnm[16] = {
        "tn", "te", "tle", "tl", "tleu", "tcs", "tneg", "tvs",
        "ta", "tne", "tg", "tge", "tgu", "tcc", "tpos", "tvc"
    };
    static const int ldsz[][2] = { { 1, 1 }, { 1, 0 }, { 2, 1 }, { 2, 0 },
                                   { 4, 0 }, { 8, 0 } };
    static const char *const ldnm[] = { "ldsb", "ldub", "ldsh", "lduh", "ld",
                                        "ldd" };
    static const char *const stnm[] = { "stb", "", "sth", "", "st", "std" };
    static const long boffs[] = { -8388608, -8388604, -4096, -8, -4, 0, 4, 8,
                                  4096, 8388600, 8388604 };
    static const long coffs[] = { -2147483647L - 1, -4096, -4, 0, 4, 8,
                                  1048576, 2147483644L };
    static const unsigned long hi22[] = { 0, 1, 2, 1023, 1024, 0x1fffff,
                                          0x200000, 0x3ffffe, 0x3fffff };
    int k, j;

    for (k = 0; k < (int)(sizeof alu / sizeof alu[0]); k++) {
        int shift = alu[k].op == SP_SLL || alu[k].op == SP_SRL ||
                    alu[k].op == SP_SRA;
        for (j = 0; j < NR; j++) {
            int d = R[j], a = R[(j + 5) % NR], b = R[(j + 11) % NR];
            V(sparc_alu(&C, alu[k].op, d, a, b), "%s %s, %s, %s",
              alu[k].nm, rn(a), rn(b), rn(d));
        }
        for (j = 0; j < NSIMM; j++) {
            long long v = shift ? (SIMM[j] & 31) : SIMM[j];
            int d = R[(j * 3 + k) % NR], a = R[(j + k + 7) % NR];
            V(sparc_alu_imm(&C, alu[k].op, d, a, v), "%s %s, %lld, %s",
              alu[k].nm, rn(a), v, rn(d));
        }
    }
    for (j = 0; j < 32; j++)
        V(sparc_alu_imm(&C, SP_SLL, R[j % NR], R[(j + 3) % NR], j),
          "sll %s, %d, %s", rn(R[(j + 3) % NR]), j, rn(R[j % NR]));
    for (j = 0; j < (int)(sizeof hi22 / sizeof hi22[0]); j++)
        V(sparc_sethi(&C, R[(j + 2) % NR], hi22[j]), "sethi %lu, %s",
          hi22[j], rn(R[(j + 2) % NR]));
    for (j = 0; j < NR; j++)
        V(sparc_mov(&C, R[j], R[(j + 4) % NR]), "or %%g0, %s, %s",
          rn(R[(j + 4) % NR]), rn(R[j]));
    for (k = 0; k < (int)(sizeof ldsz / sizeof ldsz[0]); k++)
        for (j = 0; j < NSIMM; j++) {
            int t = R[(j + k) % NR], b = R[(j * 3 + 4) % NR];
            if (ldsz[k][0] == 8)
                t &= ~1;
            V(sparc_load(&C, t, b, (int)SIMM[j], ldsz[k][0], ldsz[k][1]),
              "%s %s, %s", ldnm[k], mem(b, SIMM[j]), rn(t));
            if (*stnm[k])
                V(sparc_store(&C, t, b, (int)SIMM[j], ldsz[k][0]),
                  "%s %s, %s", stnm[k], rn(t), mem(b, SIMM[j]));
            {
                int x = R[(j + 9) % NR];
                V(sparc_load_rr(&C, t, b, x, ldsz[k][0], ldsz[k][1]),
                  "%s [%s+%s], %s", ldnm[k], rn(b), rn(x), rn(t));
                if (*stnm[k])
                    V(sparc_store_rr(&C, t, b, x, ldsz[k][0]),
                      "%s %s, [%s+%s]", stnm[k], rn(t), rn(b), rn(x));
            }
        }
    for (j = 0; j < NR; j++) {
        int r = R[j], s = R[(j + 6) % NR], u = R[(j + 12) % NR];
        V(sparc_ldstub(&C, r, s, (int)SIMM[j % NSIMM]), "ldstub %s, %s",
          mem(s, SIMM[j % NSIMM]), rn(r));
        V(sparc_swap(&C, r, s, (int)SIMM[j % NSIMM]), "swap %s, %s",
          mem(s, SIMM[j % NSIMM]), rn(r));
        V(sparc_casa(&C, s, j & 1 ? 10 : 11, u, r), "casa [%s] %d, %s, %s",
          rn(s), j & 1 ? 10 : 11, rn(u), rn(r));
        V(sparc_rdy(&C, r), "rd %%y, %s", rn(r));
        V(sparc_rdpsr(&C, r), "rd %%psr, %s", rn(r));
        V(sparc_rdwim(&C, r), "rd %%wim, %s", rn(r));
        V(sparc_rdtbr(&C, r), "rd %%tbr, %s", rn(r));
        V(sparc_wry(&C, s, r), "wr %s, %s, %%y", rn(s), rn(r));
        V(sparc_wrpsr(&C, s, r), "wr %s, %s, %%psr", rn(s), rn(r));
        V(sparc_wrwim(&C, s, r), "wr %s, %s, %%wim", rn(s), rn(r));
        V(sparc_wrtbr(&C, s, r), "wr %s, %s, %%tbr", rn(s), rn(r));
        V(sparc_jmpl(&C, r, s, SIMM[j % NSIMM]), "jmpl %s, %s",
          addr(s, SIMM[j % NSIMM]) , rn(r));
        V(sparc_save(&C, r, s, SIMM[(j + 3) % NSIMM]), "save %s, %lld, %s",
          rn(s), SIMM[(j + 3) % NSIMM], rn(r));
        V(sparc_save_rr(&C, r, s, u), "save %s, %s, %s", rn(s), rn(u), rn(r));
        V(sparc_restore(&C, r, s, u), "restore %s, %s, %s", rn(s), rn(u),
          rn(r));
        V(sparc_rett(&C, s, SIMM[j % NSIMM]), "rett %s",
          addr(s, SIMM[j % NSIMM]));
        V(sparc_flush(&C, s, (int)SIMM[j % NSIMM]), "flush %s",
          addr(s, SIMM[j % NSIMM]));
    }
    for (k = 0; k < 16; k++) {
        for (j = 0; j < (int)(sizeof boffs / sizeof boffs[0]); j++) {
            long o = boffs[(j + k) % (int)(sizeof boffs / sizeof boffs[0])];
            V(sparc_w(&C, sparc_enc_branch(k, 0, o)), "%s .%+ld", cnm[k], o);
            V(sparc_w(&C, sparc_enc_branch(k, 1, o)), "%s,a .%+ld", cnm[k], o);
        }
        V(sparc_trap(&C, k, R[k % NR], k * 8 + 7), "%s %s + %d", tnm[k],
          rn(R[k % NR]), k * 8 + 7);
        V(sparc_trap(&C, k, SP_G0, k), "%s %d", tnm[k], k);
    }
    for (j = 0; j < (int)(sizeof coffs / sizeof coffs[0]); j++)
        V(sparc_w(&C, sparc_enc_call(coffs[j])), "call .%+ld", coffs[j]);
    {
        static const unsigned long u[] = { 0, 1, 12, 4095, 4096, 0x3fffff };
        for (j = 0; j < 6; j++)
            V(sparc_unimp(&C, u[j]), "unimp %lu", u[j]);
    }
    V(sparc_nop(&C), "nop");
    V(sparc_stbar(&C), "stbar");
    V(sparc_jmpl(&C, SP_G0, SP_I7, 8), "ret");
    V(sparc_jmpl(&C, SP_G0, SP_O7, 8), "retl");
    V(sparc_jmpl(&C, SP_G0, SP_I7, 12), "jmp %%i7+12");
    V(sparc_restore(&C, SP_G0, SP_G0, SP_G0), "restore");
}

/* ---- sparc_li, executed ------------------------------------------------- */

/* The two instructions sparc_li may emit, evaluated: `or rd, rs1, simm13`
 * (rs1 %g0 or rd) and `sethi imm22, rd`. Anything else is a failure. */
static int run_li(const struct code *c, int rd, unsigned long *out)
{
    unsigned long reg[32] = { 0 };
    for (int p = 0; p + 4 <= c->len; p += 4) {
        unsigned long w = sparc_get_word(c->p + p);
        unsigned op = (unsigned)(w >> 30), d = (unsigned)(w >> 25) & 31,
                 op2 = (unsigned)(w >> 22) & 7, op3 = (unsigned)(w >> 19) & 63,
                 rs1 = (unsigned)(w >> 14) & 31, i = (unsigned)(w >> 13) & 1;
        if (op == 0 && op2 == 4) {
            reg[d] = (w & 0x3fffffUL) << 10;
        } else if (op == 2 && op3 == 2 && i) {
            unsigned long s = w & 0x1fff;
            if (s & 0x1000) s |= 0xffffe000UL;
            reg[d] = (reg[rs1] | s) & 0xffffffffUL;
        } else {
            printf("li: unexpected instruction 0x%08lx\n", w);
            return 0;
        }
        reg[0] = 0;
    }
    *out = reg[rd];
    return 1;
}

static int check_li(void)
{
    static const long long fixed[] = {
        0, 1, -1, 2, -2, 4095, 4096, -4096, -4097, 1023, 1024, 1025,
        0x3ff, 0x400, 0x12345678LL, 0x7fffffffLL, -2147483647LL - 1,
        0x80000000LL, 0xffffffffLL, 0xfffffc00LL, 0xfffff000LL, 0xffffefffLL,
        0x0000ffffLL, 0x00012000LL, 0x7ffffc00LL
    };
    int n = 0;
    unsigned long s = 12345;
    for (int k = 0; k < (int)(sizeof fixed / sizeof fixed[0]) + 4000; k++) {
        long long v;
        struct code c = { 0 };
        unsigned long got;
        if (k < (int)(sizeof fixed / sizeof fixed[0]))
            v = fixed[k];
        else {
            s = s * 1103515245UL + 12345UL;
            v = (long long)(s & 0xffffffffUL);
            if (k & 1)
                v &= (k & 2) ? 0xfffffc00LL : 0x00001fffLL;
        }
        sparc_li(&c, 9, v);
        if (!run_li(&c, 9, &got))
            return 1;
        if (got != ((unsigned long)v & 0xffffffffUL)) {
            printf("li 0x%llx computed 0x%lx\n", (unsigned long long)v, got);
            return 1;
        }
        if (c.len != sparc_li_len(v)) {
            printf("li 0x%llx is %d bytes, sparc_li_len said %d\n",
                   (unsigned long long)v, c.len, sparc_li_len(v));
            return 1;
        }
        free(c.p);
        n++;
    }
    printf("%d sparc_li sequences each compute the value asked for\n", n);
    return 0;
}

/* ---- the refusals -------------------------------------------------------- */

#define NREFUSE 16
static void refuse(int n)
{
    struct code c = { 0 };
    switch (n) {
    case 0:  sparc_alu_imm(&c, SP_ADD, 1, 2, 4096); break;
    case 1:  sparc_alu_imm(&c, SP_ADD, 1, 2, -4097); break;
    case 2:  sparc_alu_imm(&c, SP_SLL, 1, 2, 32); break;
    case 3:  sparc_load(&c, 1, 14, 4096, 4, 0); break;
    case 4:  sparc_store(&c, 1, 14, -4097, 4); break;
    case 5:  sparc_load(&c, 9, 14, 0, 8, 0); break;
    case 6:  sparc_store(&c, 1, 14, 0, 3); break;
    case 7:  sparc_w(&c, sparc_enc_branch(SP_BE, 0, 8388608)); break;
    case 8:  sparc_w(&c, sparc_enc_branch(SP_BNE, 0, -8388612)); break;
    case 9:  sparc_w(&c, sparc_enc_branch(SP_BA, 0, 6)); break;
    case 10: sparc_sethi(&c, 1, 0x400000); break;
    case 11: sparc_alu(&c, SP_ADD, 32, 1, 2); break;
    case 12: sparc_alu(&c, 0x09, 1, 2, 3); break;
    case 13: sparc_trap(&c, SP_BA, 0, 128); break;
    case 14: sparc_unimp(&c, 0x400000); break;
    case 15: sparc_casa(&c, 1, 256, 2, 3); break;
    default: printf("no refusal %d\n", n); exit(2);
    }
    printf("refusal %d did not fire; %d bytes were emitted\n", n, c.len);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--vocab")) {
        vocab();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--li"))
        return check_li();
    if (argc > 1 && !strcmp(argv[1], "--refuse")) {
        if (argc > 2 && !strcmp(argv[2], "list")) {
            printf("%d\n", NREFUSE);
            return 0;
        }
        if (argc > 2) {
            refuse(atoi(argv[2]));
            return 1;
        }
    }
    fprintf(stderr, "usage: sparccheck --vocab | --li | --refuse N|list\n");
    return 2;
}
