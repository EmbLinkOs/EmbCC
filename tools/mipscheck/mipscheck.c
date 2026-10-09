/* Hands EmbCC's whole MIPS32r2 vocabulary to llvm-mc and compares the
 * encodings, one instruction at a time.
 *
 *   mipscheck --vocab     one line per form: the assembly, a '|', and the
 *                         32-bit word src/arch/mips/emit.c produced for it
 *   mipscheck --li        executes every mips_li sequence in a small
 *                         interpreter and checks the value and the length
 *                         mips_li_len promised
 *   mipscheck --refuse N  provokes encoder range check N, which must stop
 *                         the process with an internal error;
 *                         `--refuse list` prints how many there are
 *
 * The text and the word of a line come from ONE call -- each entry below
 * formats its assembly and encodes through emit.c in the same macro -- so
 * a form printed but not encoded, or the reverse, cannot shift every
 * comparison after it and blame the wrong instruction.
 *
 * tests/golden/mips-encoding.sh feeds the text to
 * `llvm-mc -triple=mipsel-unknown-elf -mcpu=mips32r2 -show-encoding` and
 * compares each word. Branches are written with numeric displacements
 * (bytes from the delay slot), which llvm-mc encodes in place without a
 * relocation, so they are compared as bytes like everything else.
 *
 * The sweep puts every register number in every field it can occupy and
 * the immediates at both ends of both 16-bit extensions: a MIPS mistake
 * is a valid different instruction, almost never an invalid one -- an
 * andi given a sign-extended field, a shift with rs and rt the wrong way
 * round, an ext whose size field says the end bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/mips/emit.h"

static struct code C;

/* Encode, then print the text and the word just written. Each entry must
 * be exactly one instruction. */
#define V(call, ...) do {                                                   \
        int at_ = C.len;                                                    \
        char txt_[160];                                                     \
        call;                                                               \
        if (C.len - at_ != 4) {                                             \
            fprintf(stderr, "mipscheck: an entry emitted %d bytes\n",       \
                    C.len - at_);                                           \
            exit(2);                                                        \
        }                                                                   \
        snprintf(txt_, sizeof txt_, __VA_ARGS__);                           \
        if (mips_big_endian())          /* the bytes in memory order */     \
            printf("%s|%02x%02x%02x%02x\n", txt_, C.p[at_], C.p[at_ + 1],   \
                   C.p[at_ + 2], C.p[at_ + 3]);                             \
        else                            /* the word, its high byte first */ \
            printf("%s|%02x%02x%02x%02x\n", txt_, C.p[at_ + 3],             \
                   C.p[at_ + 2], C.p[at_ + 1], C.p[at_]);                   \
    } while (0)

static const int R[] = { 0, 1, 2, 3, 4, 5, 7, 8, 12, 15, 16, 21, 23, 24,
                         25, 26, 28, 29, 30, 31 };
#define NR ((int)(sizeof R / sizeof R[0]))
static const long long SIMM[] = { -32768, -32767, -256, -2, -1, 0, 1, 2,
                                  255, 256, 4095, 32766, 32767 };
#define NSIMM ((int)(sizeof SIMM / sizeof SIMM[0]))
static const long long UIMM[] = { 0, 1, 255, 256, 4095, 32767, 32768,
                                  65534, 65535 };
#define NUIMM ((int)(sizeof UIMM / sizeof UIMM[0]))

static void vocab(void)
{
    static const struct { int op; const char *nm; } alu[] = {
        { MIPS_ADDU, "addu" }, { MIPS_SUBU, "subu" }, { MIPS_AND, "and" },
        { MIPS_OR, "or" }, { MIPS_XOR, "xor" }, { MIPS_NOR, "nor" },
        { MIPS_SLT, "slt" }, { MIPS_SLTU, "sltu" }, { MIPS_MOVN, "movn" },
        { MIPS_MOVZ, "movz" }, { MIPS_MUL, "mul" },
        /* source order is (value, amount); the assembly is too */
        { MIPS_SLLV, "sllv" }, { MIPS_SRLV, "srlv" }, { MIPS_SRAV, "srav" },
        { MIPS_ROTRV, "rotrv" }
    };
    static const struct { int op; const char *nm; int sign; } aimm[] = {
        { MIPS_ADDIU, "addiu", 1 }, { MIPS_SLTI, "slti", 1 },
        { MIPS_SLTIU, "sltiu", 1 }, { MIPS_ANDI, "andi", 0 },
        { MIPS_ORI, "ori", 0 }, { MIPS_XORI, "xori", 0 }
    };
    static const struct { int op; const char *nm; } shi[] = {
        { MIPS_SLL, "sll" }, { MIPS_SRL, "srl" }, { MIPS_SRA, "sra" },
        { MIPS_ROTR, "rotr" }
    };
    static const struct { int op; const char *nm; } md[] = {
        { MIPS_MULT, "mult" }, { MIPS_MULTU, "multu" },
        /* `div $zero, a, b` is the instruction; plain `div a, b` is a
         * macro that inserts a divide-by-zero trap */
        { MIPS_DIV, "div $zero," }, { MIPS_DIVU, "divu $zero," }
    };
    static const int sas[] = { 0, 1, 2, 15, 16, 31 };
    static const long long offs[] = { -32768, -4, -1, 0, 1, 4, 255, 32767 };
    static const int ldsz[][2] = { { 1, 1 }, { 1, 0 }, { 2, 1 }, { 2, 0 },
                                   { 4, 1 } };
    static const char *const ldnm[] = { "lb", "lbu", "lh", "lhu", "lw" };
    static const char *const stnm[] = { "sb", "", "sh", "", "sw" };
    static const int bit[][2] = { { 0, 1 }, { 0, 32 }, { 31, 1 }, { 4, 8 },
                                  { 16, 16 }, { 1, 31 }, { 7, 25 } };
    static const long boffs[] = { -131072, -131068, -8, -4, 0, 4, 8, 4096,
                                  131068 };
    int k, j;

    for (k = 0; k < (int)(sizeof alu / sizeof alu[0]); k++)
        for (j = 0; j < NR; j++) {
            int d = R[j], a = R[(j + 5) % NR], b = R[(j + 11) % NR];
            V(mips_alu(&C, alu[k].op, d, a, b), "%s $%d, $%d, $%d",
              alu[k].nm, d, a, b);
        }
    for (k = 0; k < (int)(sizeof aimm / sizeof aimm[0]); k++) {
        int n = aimm[k].sign ? NSIMM : NUIMM;
        for (j = 0; j < n; j++) {
            long long v = aimm[k].sign ? SIMM[j] : UIMM[j];
            int t = R[j % NR], s = R[(j + 7) % NR];
            V(mips_alu_imm(&C, aimm[k].op, t, s, v), "%s $%d, $%d, %lld",
              aimm[k].nm, t, s, v);
        }
    }
    for (j = 0; j < NUIMM; j++)
        V(mips_lui(&C, R[j % NR], (unsigned)UIMM[j]), "lui $%d, %lld",
          R[j % NR], UIMM[j]);
    for (k = 0; k < (int)(sizeof shi / sizeof shi[0]); k++)
        for (j = 0; j < (int)(sizeof sas / sizeof sas[0]); j++) {
            int d = R[(j * 3 + k) % NR], t = R[(j * 5 + 2) % NR];
            V(mips_shift_imm(&C, shi[k].op, d, t, sas[j]), "%s $%d, $%d, %d",
              shi[k].nm, d, t, sas[j]);
        }
    for (k = 0; k < (int)(sizeof md / sizeof md[0]); k++)
        for (j = 0; j < NR; j++) {
            int a = R[j], b = R[(j + 3) % NR];
            V(mips_muldiv(&C, md[k].op, a, b), "%s $%d, $%d", md[k].nm, a, b);
        }
    for (j = 0; j < NR; j++) {
        int r = R[j], s = R[(j + 9) % NR];
        V(mips_mfhi(&C, r), "mfhi $%d", r);
        V(mips_mflo(&C, r), "mflo $%d", r);
        V(mips_mthi(&C, r), "mthi $%d", r);
        V(mips_mtlo(&C, r), "mtlo $%d", r);
        V(mips_clz(&C, r, s), "clz $%d, $%d", r, s);
        V(mips_clo(&C, r, s), "clo $%d, $%d", r, s);
        V(mips_seb(&C, r, s), "seb $%d, $%d", r, s);
        V(mips_seh(&C, r, s), "seh $%d, $%d", r, s);
        V(mips_wsbh(&C, r, s), "wsbh $%d, $%d", r, s);
        V(mips_mv(&C, r, s), "or $%d, $%d, $0", r, s);
        V(mips_jr(&C, r), "jr $%d", r);
        V(mips_jalr(&C, s == r ? 31 : s, r), "jalr $%d, $%d", s == r ? 31 : s, r);
    }
    for (k = 0; k < (int)(sizeof bit / sizeof bit[0]); k++) {
        int t = R[(k + 2) % NR], s = R[(k + 13) % NR];
        V(mips_ext(&C, t, s, bit[k][0], bit[k][1]), "ext $%d, $%d, %d, %d",
          t, s, bit[k][0], bit[k][1]);
        V(mips_ins(&C, t, s, bit[k][0], bit[k][1]), "ins $%d, $%d, %d, %d",
          t, s, bit[k][0], bit[k][1]);
    }
    for (k = 0; k < (int)(sizeof ldsz / sizeof ldsz[0]); k++)
        for (j = 0; j < (int)(sizeof offs / sizeof offs[0]); j++) {
            int t = R[(j + k) % NR], b = R[(j * 3 + 4) % NR];
            V(mips_load(&C, t, b, (int)offs[j], ldsz[k][0], ldsz[k][1]),
              "%s $%d, %lld($%d)", ldnm[k], t, offs[j], b);
            if (*stnm[k])
                V(mips_store(&C, t, b, (int)offs[j], ldsz[k][0]),
                  "%s $%d, %lld($%d)", stnm[k], t, offs[j], b);
        }
    for (j = 0; j < (int)(sizeof offs / sizeof offs[0]); j++) {
        int t = R[(j + 6) % NR], b = R[(j + 1) % NR];
        V(mips_ll(&C, t, b, (int)offs[j]), "ll $%d, %lld($%d)", t, offs[j], b);
        V(mips_lwl(&C, t, b, (int)offs[j]), "lwl $%d, %lld($%d)", t, offs[j], b);
        V(mips_lwr(&C, t, b, (int)offs[j]), "lwr $%d, %lld($%d)", t, offs[j], b);
        V(mips_swl(&C, t, b, (int)offs[j]), "swl $%d, %lld($%d)", t, offs[j], b);
        V(mips_swr(&C, t, b, (int)offs[j]), "swr $%d, %lld($%d)", t, offs[j], b);
        V(mips_sc(&C, t, b, (int)offs[j]), "sc $%d, %lld($%d)", t, offs[j], b);
    }
    V(mips_sync(&C, 0), "sync");
    V(mips_sync(&C, 4), "sync 4");
    for (j = 0; j < (int)(sizeof boffs / sizeof boffs[0]); j++) {
        int a = R[(j + 1) % NR], b = R[(j + 8) % NR];
        long o = boffs[j];
        V(mips_w(&C, mips_enc_branch(MIPS_BEQ, a, b, o)),
          "beq $%d, $%d, %ld", a, b, o);
        V(mips_w(&C, mips_enc_branch(MIPS_BNE, a, b, o)),
          "bne $%d, $%d, %ld", a, b, o);
        V(mips_w(&C, mips_enc_branch(MIPS_BLEZ, a, 0, o)), "blez $%d, %ld", a, o);
        V(mips_w(&C, mips_enc_branch(MIPS_BGTZ, a, 0, o)), "bgtz $%d, %ld", a, o);
        V(mips_w(&C, mips_enc_branch(MIPS_BLTZ, a, 0, o)), "bltz $%d, %ld", a, o);
        V(mips_w(&C, mips_enc_branch(MIPS_BGEZ, a, 0, o)), "bgez $%d, %ld", a, o);
        V(mips_w(&C, mips_enc_branch(MIPS_BAL, 0, 0, o)), "bal %ld", o);
    }
    V(mips_jal(&C), "jal 0");
    V(mips_j(&C), "j 0");
    V(mips_nop(&C), "nop");
    V(mips_ehb(&C), "ehb");
    V(mips_syscall(&C), "syscall");
    V(mips_eret(&C), "eret");
    V(mips_wait(&C), "wait");
    {
        static const int codes[] = { 0, 1, 7, 512, 1023 };
        for (j = 0; j < 5; j++) {
            V(mips_break(&C, codes[j]), "break %d", codes[j]);
            V(mips_teq(&C, R[j + 3], R[j + 9], codes[j]), "teq $%d, $%d, %d",
              R[j + 3], R[j + 9], codes[j]);
        }
    }
    for (j = 0; j < NR; j++) {
        int t = R[j], d = R[(j + 4) % NR], sel = j % 8;
        V(mips_mfc0(&C, t, d, sel), "mfc0 $%d, $%d, %d", t, d, sel);
        V(mips_mtc0(&C, t, d, sel), "mtc0 $%d, $%d, %d", t, d, sel);
        V(mips_di(&C, t), "di $%d", t);
        V(mips_ei(&C, t), "ei $%d", t);
    }
}

/* MIPS64's doubleword forms (--64): the shifts at every amount class
 * (below 32, the *32 forms, 63), the three field encodings of dext and
 * dins at their edges, the doubleword loads and stores. */
static void vocab64(void)
{
    static const struct { int op; const char *nm; } alu[] = {
        { MIPS_DADDU, "daddu" }, { MIPS_DSUBU, "dsubu" },
        { MIPS_DSLLV, "dsllv" }, { MIPS_DSRLV, "dsrlv" },
        { MIPS_DSRAV, "dsrav" }, { MIPS_DROTRV, "drotrv" }
    };
    static const struct { int op; const char *nm; } shi[] = {
        { MIPS_DSLL, "dsll" }, { MIPS_DSRL, "dsrl" }, { MIPS_DSRA, "dsra" },
        { MIPS_DROTR, "drotr" }
    };
    static const struct { int op; const char *nm; } md[] = {
        { MIPS_DMULT, "dmult" }, { MIPS_DMULTU, "dmultu" },
        { MIPS_DDIV, "ddiv $zero," }, { MIPS_DDIVU, "ddivu $zero," }
    };
    static const int sas[] = { 0, 1, 16, 31, 32, 33, 40, 63 };
    static const long long offs[] = { -32768, -8, -1, 0, 1, 8, 255, 32767 };
    /* (pos, size): dext, dextm (size > 32), dextu (pos >= 32), and dins,
     * dinsm (end >= 32), dinsu */
    static const int bit[][2] = { { 0, 1 }, { 0, 32 }, { 31, 1 }, { 4, 8 },
                                  { 0, 33 }, { 0, 64 }, { 31, 33 },
                                  { 8, 40 }, { 32, 1 }, { 32, 32 },
                                  { 63, 1 }, { 40, 16 }, { 16, 32 },
                                  { 1, 31 }, { 31, 2 } };
    int k, j;
    for (k = 0; k < (int)(sizeof alu / sizeof alu[0]); k++)
        for (j = 0; j < NR; j++) {
            int d = R[j], a = R[(j + 5) % NR], b = R[(j + 11) % NR];
            V(mips_alu(&C, alu[k].op, d, a, b), "%s $%d, $%d, $%d",
              alu[k].nm, d, a, b);
        }
    for (j = 0; j < NSIMM; j++) {
        int t = R[j % NR], s = R[(j + 7) % NR];
        V(mips_alu_imm(&C, MIPS_DADDIU, t, s, SIMM[j]), "daddiu $%d, $%d, %lld",
          t, s, SIMM[j]);
    }
    for (k = 0; k < (int)(sizeof shi / sizeof shi[0]); k++)
        for (j = 0; j < (int)(sizeof sas / sizeof sas[0]); j++) {
            int d = R[(j * 3 + k) % NR], t = R[(j * 5 + 2) % NR];
            V(mips_shift_imm(&C, shi[k].op, d, t, sas[j]), "%s $%d, $%d, %d",
              shi[k].nm, d, t, sas[j]);
        }
    for (k = 0; k < (int)(sizeof md / sizeof md[0]); k++)
        for (j = 0; j < NR; j++) {
            int a = R[j], b = R[(j + 3) % NR];
            V(mips_muldiv(&C, md[k].op, a, b), "%s $%d, $%d", md[k].nm, a, b);
        }
    for (j = 0; j < NR; j++) {
        int r = R[j], s = R[(j + 9) % NR], d = R[(j + 4) % NR];
        V(mips_dclz(&C, r, s), "dclz $%d, $%d", r, s);
        V(mips_dclo(&C, r, s), "dclo $%d, $%d", r, s);
        V(mips_dsbh(&C, r, s), "dsbh $%d, $%d", r, s);
        V(mips_dshd(&C, r, s), "dshd $%d, $%d", r, s);
        V(mips_dmfc0(&C, r, d, j % 8), "dmfc0 $%d, $%d, %d", r, d, j % 8);
        V(mips_dmtc0(&C, r, d, j % 8), "dmtc0 $%d, $%d, %d", r, d, j % 8);
    }
    for (k = 0; k < (int)(sizeof bit / sizeof bit[0]); k++) {
        int t = R[(k + 2) % NR], s = R[(k + 13) % NR];
        V(mips_dext(&C, t, s, bit[k][0], bit[k][1]), "%s $%d, $%d, %d, %d",
          bit[k][0] >= 32 ? "dextu" : bit[k][1] > 32 ? "dextm" : "dext",
          t, s, bit[k][0], bit[k][1]);
        V(mips_dins(&C, t, s, bit[k][0], bit[k][1]), "%s $%d, $%d, %d, %d",
          bit[k][0] >= 32 ? "dinsu" : bit[k][0] + bit[k][1] > 32 ? "dinsm"
                                                               : "dins",
          t, s, bit[k][0], bit[k][1]);
    }
    for (j = 0; j < (int)(sizeof offs / sizeof offs[0]); j++) {
        int t = R[(j + 6) % NR], b = R[(j + 1) % NR];
        V(mips_load(&C, t, b, (int)offs[j], 8, 1), "ld $%d, %lld($%d)", t, offs[j], b);
        V(mips_store(&C, t, b, (int)offs[j], 8), "sd $%d, %lld($%d)", t, offs[j], b);
        V(mips_lwu(&C, t, b, (int)offs[j]), "lwu $%d, %lld($%d)", t, offs[j], b);
        V(mips_ldl(&C, t, b, (int)offs[j]), "ldl $%d, %lld($%d)", t, offs[j], b);
        V(mips_ldr(&C, t, b, (int)offs[j]), "ldr $%d, %lld($%d)", t, offs[j], b);
        V(mips_sdl(&C, t, b, (int)offs[j]), "sdl $%d, %lld($%d)", t, offs[j], b);
        V(mips_sdr(&C, t, b, (int)offs[j]), "sdr $%d, %lld($%d)", t, offs[j], b);
        V(mips_lld(&C, t, b, (int)offs[j]), "lld $%d, %lld($%d)", t, offs[j], b);
        V(mips_scd(&C, t, b, (int)offs[j]), "scd $%d, %lld($%d)", t, offs[j], b);
    }
}

/* ---- mips_li, executed ------------------------------------------------ */

/* The three instructions mips_li may emit, evaluated: addiu rd, $0, imm;
 * ori rd, rs, imm (rs $0 or rd); lui rd, imm. Anything else is a failure,
 * because then the sequence is not the one this mode understands. */
static int run_li(const struct code *c, int rd, unsigned long *out)
{
    unsigned long reg[32] = { 0 };
    for (int p = 0; p + 4 <= c->len; p += 4) {
        unsigned long w = mips_get_word(c->p + p);
        unsigned op = (unsigned)(w >> 26), rs = (unsigned)(w >> 21) & 31,
                 rt = (unsigned)(w >> 16) & 31, imm = (unsigned)w & 0xffff;
        unsigned long simm = imm & 0x8000 ? (0xffff0000UL | imm) : imm;
        switch (op) {
        case 0x09: reg[rt] = (reg[rs] + simm) & 0xffffffffUL; break;  /* addiu */
        case 0x0d: reg[rt] = reg[rs] | imm; break;                     /* ori */
        case 0x0f: reg[rt] = (unsigned long)imm << 16; break;          /* lui */
        default:
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
        0, 1, -1, 2, -2, 32767, 32768, -32768, -32769, 65535, 65536,
        65537, 0x10000, 0x12340000LL, 0x12345678LL, 0x7fffffffLL,
        -2147483647LL - 1, 0x80000000LL, 0xffffffffLL, 0xffff0000LL,
        0xffff8000LL, 0xffff7fffLL, 0x0000ffffLL, 0x00018000LL, 0x7fff8000LL
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
                v &= (k & 2) ? 0xffff0000LL : 0x0001ffffLL;
        }
        mips_li(&c, 9, v);
        if (!run_li(&c, 9, &got))
            return 1;
        if (got != ((unsigned long)v & 0xffffffffUL)) {
            printf("li 0x%llx computed 0x%lx\n", (unsigned long long)v, got);
            return 1;
        }
        if (c.len != mips_li_len(v)) {
            printf("li 0x%llx is %d bytes, mips_li_len said %d\n",
                   (unsigned long long)v, c.len, mips_li_len(v));
            return 1;
        }
        free(c.p);
        n++;
    }
    printf("%d mips_li sequences each compute the value asked for\n", n);
    return 0;
}

/* mips_li64, executed: a 64-bit register file and the instructions
 * mips_li64 may emit -- lui, addiu, ori, dsll(32), dext 0,32 -- with the
 * MIPS64 semantics (lui and addiu sign-extend their 32-bit result). */
static int run_li64(const struct code *c, int rd, unsigned long long *out)
{
    unsigned long long reg[32] = { 0 };
    for (int p = 0; p + 4 <= c->len; p += 4) {
        unsigned long w = mips_get_word(c->p + p);
        unsigned op = (unsigned)(w >> 26), rs = (unsigned)(w >> 21) & 31,
                 rt = (unsigned)(w >> 16) & 31, rdd = (unsigned)(w >> 11) & 31,
                 sa = (unsigned)(w >> 6) & 31, fn = (unsigned)w & 63,
                 imm = (unsigned)w & 0xffff;
        long long simm = (short)imm;
        if (op == 0x09) {                                  /* addiu */
            reg[rt] = (unsigned long long)(long long)(int)(unsigned)
                      (reg[rs] + (unsigned long long)simm);
        } else if (op == 0x0d) {                           /* ori */
            reg[rt] = reg[rs] | imm;
        } else if (op == 0x0f) {                           /* lui */
            reg[rt] = (unsigned long long)(long long)(int)(imm << 16);
        } else if (op == 0 && rs == 0 && fn == 0x38) {     /* dsll */
            reg[rdd] = reg[rt] << sa;
        } else if (op == 0 && rs == 0 && fn == 0x3c) {     /* dsll32 */
            reg[rdd] = reg[rt] << (sa + 32);
        } else if (op == 0x1f && fn == 0x03 && sa == 0 && rdd == 31) {
            reg[rt] = reg[rs] & 0xffffffffULL;             /* dext 0, 32 */
        } else {
            printf("li64: unexpected instruction 0x%08lx\n", w);
            return 0;
        }
        reg[0] = 0;
    }
    *out = reg[rd];
    return 1;
}

static int check_li64(void)
{
    static const long long fixed[] = {
        0, 1, -1, 0x7fffffffLL, 0x80000000LL, 0xffffffffLL, 0x100000000LL,
        -2147483649LL, 0x123456789abcdef0LL, (long long)0x8000000000000000ULL,
        0x7fffffffffffffffLL, 0xffffffff80100000LL, 0x0000ffff00000000LL,
        0x00000000ffff0000LL, 0x0001000000000001LL, 0x1234000000000000LL,
        (long long)0xfedcba9876543210ULL, 0x00000000bf000900LL
    };
    int n = 0, nfix = (int)(sizeof fixed / sizeof fixed[0]);
    unsigned long long s = 88172645463325252ULL;
    for (int k = 0; k < nfix + 20000; k++) {
        long long v;
        struct code c = { 0 };
        unsigned long long got;
        if (k < nfix) {
            v = fixed[k];
        } else {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            v = (long long)s;
            switch (k & 7) {             /* the shapes that take shortcuts */
            case 1: v &= 0xffffffffLL; break;
            case 2: v = (long long)((unsigned long long)v << (k % 48)); break;
            case 3: v >>= (k % 60); break;
            case 4: v &= (long long)0xffff0000ffff0000ULL; break;
            default: break;
            }
        }
        mips_li64(&c, 9, v);
        if (!run_li64(&c, 9, &got))
            return 1;
        if (got != (unsigned long long)v) {
            printf("li64 0x%llx computed 0x%llx\n", (unsigned long long)v, got);
            return 1;
        }
        if (c.len != mips_li64_len(v) || c.len > 24) {
            printf("li64 0x%llx is %d bytes, mips_li64_len said %d\n",
                   (unsigned long long)v, c.len, mips_li64_len(v));
            return 1;
        }
        free(c.p);
        n++;
    }
    printf("%d mips_li64 sequences each compute the value asked for, in at "
           "most six instructions\n", n);
    return 0;
}

/* ---- the refusals ----------------------------------------------------- */

/* Every range check is load-bearing: the field is narrower than the C
 * type the caller passes, and a truncated value is a real instruction that
 * does something else. Each is provoked here by number, and the golden
 * test checks the process stops rather than emits. */
#define NREFUSE 21
/* ...and with --64, the doubleword forms' own checks (NREFUSE64 more),
 * while the first NREFUSE are run with the switch OFF -- where every
 * doubleword instruction must be refused too, the cases from 21 on. */
#define NREFUSE64 14
static void refuse(int n)
{
    struct code c = { 0 };
    mips_set_64(n >= NREFUSE + 7);     /* the MIPS32 checks with it off */
    switch (n) {
    /* the switch off: a doubleword instruction in a MIPS32 object */
    case 21: mips_load(&c, 2, 29, 0, 8, 1); break;
    case 22: mips_alu(&c, MIPS_DADDU, 2, 3, 4); break;
    case 23: mips_shift_imm(&c, MIPS_DSLL, 2, 3, 1); break;
    case 24: mips_alu_imm(&c, MIPS_DADDIU, 2, 3, 1); break;
    case 25: mips_dext(&c, 2, 3, 0, 32); break;
    case 26: mips_li64(&c, 2, 0x100000000LL); break;
    case 27: mips_muldiv(&c, MIPS_DMULTU, 2, 3); break;
    /* the switch on: the doubleword ranges */
    case 28: mips_shift_imm(&c, MIPS_DSLL, 2, 3, 64); break;
    case 29: mips_dext(&c, 2, 3, 32, 33); break;
    case 30: mips_dins(&c, 2, 3, 1, 64); break;
    case 31: mips_dext(&c, 2, 3, 0, 0); break;
    case 32: mips_alu_imm(&c, MIPS_DADDIU, 2, 3, 32768); break;
    case 33: mips_ldl(&c, 2, 29, -32769); break;
    case 34: mips_load(&c, 2, 29, 0, 16, 1); break;
    case 0:  mips_alu_imm(&c, MIPS_ADDIU, 2, 3, 32768); break;
    case 1:  mips_alu_imm(&c, MIPS_ADDIU, 2, 3, -32769); break;
    case 2:  mips_alu_imm(&c, MIPS_ANDI, 2, 3, -1); break;
    case 3:  mips_alu_imm(&c, MIPS_ORI, 2, 3, 65536); break;
    case 4:  mips_alu_imm(&c, MIPS_SLTIU, 2, 3, 40000); break;
    case 5:  mips_load(&c, 2, 29, 32768, 4, 1); break;
    case 6:  mips_store(&c, 2, 29, -32769, 4); break;
    case 7:  mips_load(&c, 2, 29, 0, 8, 1); break;
    case 8:  mips_shift_imm(&c, MIPS_SLL, 2, 3, 32); break;
    case 9:  mips_w(&c, mips_enc_branch(MIPS_BEQ, 2, 3, 131072)); break;
    case 10: mips_w(&c, mips_enc_branch(MIPS_BNE, 2, 3, -131076)); break;
    case 11: mips_w(&c, mips_enc_branch(MIPS_BEQ, 2, 3, 6)); break;
    case 12: mips_ext(&c, 2, 3, 4, 29); break;
    case 13: mips_ins(&c, 2, 3, 0, 0); break;
    case 14: mips_break(&c, 1024); break;
    case 15: mips_mfc0(&c, 2, 12, 8); break;
    case 16: mips_alu(&c, MIPS_ADDU, 32, 2, 3); break;
    case 17: mips_w(&c, mips_enc_j(3, 0x4000000UL)); break;
    case 18: mips_lui(&c, 2, 0x10000); break;
    case 19: mips_store(&c, 2, 29, 0, 3); break;
    case 20: mips_lwl(&c, 2, 29, 32768); break;
    default: printf("no refusal %d\n", n); exit(2);
    }
    printf("refusal %d did not fire; %d bytes were emitted\n", n, c.len);
    exit(1);
}

int main(int argc, char **argv)
{
    /* --be: big-endian (mips-none-elf), the bytes compared in memory
     * order against llvm-mc's for mips-unknown-elf */
    if (argc > 1 && !strcmp(argv[1], "--be")) {
        mips_set_big_endian(1);
        argc--;
        argv++;
    }
    /* --64: MIPS64r2 (mips64el, mips64): the doubleword forms too */
    if (argc > 1 && !strcmp(argv[1], "--64")) {
        mips_set_64(1);
        argc--;
        argv++;
    }
    if (argc > 1 && !strcmp(argv[1], "--vocab")) {
        vocab();
        if (mips_is_64())
            vocab64();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--li"))
        return mips_is_64() ? check_li64() : check_li();
    if (argc > 1 && !strcmp(argv[1], "--refuse")) {
        if (argc > 2 && !strcmp(argv[2], "list")) {
            printf("%d\n", mips_is_64() ? NREFUSE + NREFUSE64 : NREFUSE);
            return 0;
        }
        refuse(argc > 2 ? atoi(argv[2]) : -1);
    }
    fprintf(stderr, "usage: mipscheck [--be] [--64] --vocab | --li | --refuse N|list\n");
    return 2;
}
