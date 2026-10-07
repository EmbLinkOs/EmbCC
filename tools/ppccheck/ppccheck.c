/* Hands EmbCC's whole PowerPC vocabulary to llvm-mc and compares the
 * encodings, one instruction at a time.
 *
 *   ppccheck --vocab     one line per form: the assembly in llvm-mc's
 *                        spelling, a '|', and the four bytes
 *                        src/arch/ppc/emit.c stored, in memory order
 *   ppccheck --li        executes every ppc_li sequence in a small
 *                        interpreter and checks the value and the length
 *                        ppc_li_len promised
 *   ppccheck --refuse N  provokes encoder range check N, which must stop
 *                        the process with an internal error;
 *                        `--refuse list` prints how many there are
 *
 * The text and the bytes of a line come from ONE call -- each entry
 * formats its assembly and encodes through emit.c in the same macro -- so
 * a form printed but not encoded cannot shift every comparison after it.
 *
 * The assembly is written in the ISA's operand order, which is NOT the
 * source order emit.c takes: `subf d, b, a` is ppc_alu(PPC_SUB, d, a, b),
 * and the logical forms name their destination second in the encoding.
 * That difference is exactly what this test exists to catch, so the text
 * is spelled out by hand per form rather than derived from the call.
 * Branches are written `b (N)`, a displacement, which llvm-mc encodes in place. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/ppc/emit.h"

static struct code C;

#define V(call, ...) do {                                                   \
        int at_ = C.len;                                                    \
        char txt_[160];                                                     \
        call;                                                               \
        if (C.len - at_ != 4) {                                             \
            fprintf(stderr, "ppccheck: an entry emitted %d bytes\n",        \
                    C.len - at_);                                           \
            exit(2);                                                        \
        }                                                                   \
        snprintf(txt_, sizeof txt_, __VA_ARGS__);                           \
        printf("%s|%02x%02x%02x%02x\n", txt_, C.p[at_], C.p[at_ + 1],       \
               C.p[at_ + 2], C.p[at_ + 3]);                                 \
    } while (0)

static const int R[] = { 0, 1, 2, 3, 4, 5, 7, 8, 10, 11, 12, 13, 14, 16,
                         21, 25, 28, 29, 30, 31 };
#define NR ((int)(sizeof R / sizeof R[0]))
static const long long SIMM[] = { -32768, -32767, -256, -2, -1, 0, 1, 2,
                                  255, 256, 4095, 32766, 32767 };
#define NSIMM ((int)(sizeof SIMM / sizeof SIMM[0]))
static const long long UIMM[] = { 0, 1, 255, 256, 4095, 32767, 32768,
                                  65534, 65535 };
#define NUIMM ((int)(sizeof UIMM / sizeof UIMM[0]))

/* A register other than r0, for the fields where r0 means 0. */
static int nz(int r) { return r ? r : 9; }

static void vocab(void)
{
    /* d = a OP b, spelt in the ISA's order */
    static const struct { int op; const char *nm; int swap; } alu[] = {
        { PPC_ADD, "add", 0 }, { PPC_SUB, "subf", 1 },
        { PPC_ADDC, "addc", 0 }, { PPC_ADDE, "adde", 0 },
        { PPC_SUBC, "subfc", 1 }, { PPC_SUBE, "subfe", 1 },
        { PPC_MULLW, "mullw", 0 }, { PPC_MULHW, "mulhw", 0 },
        { PPC_MULHWU, "mulhwu", 0 }, { PPC_DIVW, "divw", 0 },
        { PPC_DIVWU, "divwu", 0 }, { PPC_AND, "and", 0 },
        { PPC_ANDC, "andc", 0 }, { PPC_OR, "or", 0 }, { PPC_ORC, "orc", 0 },
        { PPC_XOR, "xor", 0 }, { PPC_NAND, "nand", 0 }, { PPC_NOR, "nor", 0 },
        { PPC_EQV, "eqv", 0 }, { PPC_SLW, "slw", 0 }, { PPC_SRW, "srw", 0 },
        { PPC_SRAW, "sraw", 0 }
    };
    static const struct { int op; const char *nm; } un[] = {
        { PPC_NEG, "neg" }, { PPC_ADDZE, "addze" }, { PPC_SUBFZE, "subfze" },
        { PPC_ADDME, "addme" }, { PPC_SUBFME, "subfme" },
        { PPC_CNTLZW, "cntlzw" }, { PPC_EXTSB, "extsb" },
        { PPC_EXTSH, "extsh" }
    };
    static const struct { int op; const char *nm; int sign; } im[] = {
        { PPC_ADDI, "addi", 1 }, { PPC_ADDIS, "addis", 1 },
        { PPC_ADDIC, "addic", 1 }, { PPC_MULLI, "mulli", 1 },
        { PPC_SUBFIC, "subfic", 1 }, { PPC_ORI, "ori", 0 },
        { PPC_ORIS, "oris", 0 }, { PPC_XORI, "xori", 0 },
        { PPC_XORIS, "xoris", 0 }, { PPC_ANDI_, "andi.", 0 },
        { PPC_ANDIS_, "andis.", 0 }
    };
    static const char *const cname[] = { "lt", "ge", "gt", "le", "eq", "ne" };
    int j, k;

    for (k = 0; k < (int)(sizeof alu / sizeof alu[0]); k++)
        for (j = 0; j < NR; j++) {
            int d = R[j], a = R[(j + 5) % NR], b = R[(j + 11) % NR];
            V(ppc_alu(&C, alu[k].op, d, a, b), "%s %d, %d, %d", alu[k].nm, d,
              alu[k].swap ? b : a, alu[k].swap ? a : b);
        }
    for (k = 0; k < (int)(sizeof un / sizeof un[0]); k++)
        for (j = 0; j < NR; j++) {
            int d = R[j], s = R[(j + 7) % NR];
            V(ppc_un(&C, un[k].op, d, s), "%s %d, %d", un[k].nm, d, s);
        }
    for (k = 0; k < (int)(sizeof im / sizeof im[0]); k++) {
        int n = im[k].sign ? NSIMM : NUIMM;
        for (j = 0; j < n; j++) {
            long long v = im[k].sign ? SIMM[j] : UIMM[j];
            int d = R[j % NR], s = R[(j + 3) % NR];
            if (im[k].op == PPC_ADDI || im[k].op == PPC_ADDIS)
                s = nz(s);
            V(ppc_imm(&C, im[k].op, d, s, v), "%s %d, %d, %lld", im[k].nm, d,
              s, v);
        }
    }
    for (j = 0; j < NR; j++) {
        int d = R[j], s = R[(j + 3) % NR];
        V(ppc_mr(&C, d, s), "or %d, %d, %d", d, s, s);
        V(ppc_srawi(&C, d, s, j), "srawi %d, %d, %d", d, s, j);
        V(ppc_rlwinm(&C, d, s, j, (j * 7) % 32, (j * 13) % 32),
          "rlwinm %d, %d, %d, %d, %d", d, s, j, (j * 7) % 32, (j * 13) % 32);
        V(ppc_rlwimi(&C, d, s, 31 - j, (j * 5) % 32, (j * 3) % 32),
          "rlwimi %d, %d, %d, %d, %d", d, s, 31 - j, (j * 5) % 32,
          (j * 3) % 32);
        V(ppc_rlwnm(&C, d, s, R[(j + 9) % NR], j, 31 - j),
          "rlwnm %d, %d, %d, %d, %d", d, s, R[(j + 9) % NR], j, 31 - j);
        V(ppc_slwi(&C, d, s, j + 1), "rlwinm %d, %d, %d, 0, %d", d, s, j + 1,
          31 - (j + 1));
        V(ppc_srwi(&C, d, s, j + 1), "rlwinm %d, %d, %d, %d, 31", d, s,
          31 - j, j + 1);
    }
    for (j = 0; j < NSIMM; j++)
        V(ppc_li(&C, R[j % NR], SIMM[j]), "li %d, %lld", R[j % NR], SIMM[j]);
    for (j = 0; j < NUIMM; j++)
        V(ppc_lis(&C, R[j % NR], (unsigned)UIMM[j]), "lis %d, %lld",
          R[j % NR], UIMM[j] > 32767 ? UIMM[j] - 65536 : UIMM[j]);
    for (j = 0; j < NR; j++) {
        int a = R[j], b = R[(j + 4) % NR], cr = j % 8;
        V(ppc_cmp(&C, cr, 1, a, b), "cmpw %d, %d, %d", cr, a, b);
        V(ppc_cmp(&C, cr, 0, a, b), "cmplw %d, %d, %d", cr, a, b);
        V(ppc_cmpi(&C, cr, 1, a, SIMM[j % NSIMM]), "cmpwi %d, %d, %lld", cr,
          a, SIMM[j % NSIMM]);
        V(ppc_cmpi(&C, cr, 0, a, UIMM[j % NUIMM]), "cmplwi %d, %d, %lld", cr,
          a, UIMM[j % NUIMM]);
        V(ppc_mfcr(&C, a), "mfcr %d", a);
        V(ppc_crxor(&C, j, (j + 3) % 32, (j * 5) % 32), "crxor %d, %d, %d", j,
          (j + 3) % 32, (j * 5) % 32);
    }
    /* memory */
    for (j = 0; j < NR; j++) {
        int t = R[j], b = nz(R[(j + 6) % NR]), x = R[(j + 9) % NR];
        int off = (int)SIMM[j % NSIMM];
        V(ppc_load(&C, t, b, off, 1, 0), "lbz %d, %d(%d)", t, off, b);
        V(ppc_load(&C, t, b, off, 2, 0), "lhz %d, %d(%d)", t, off, b);
        V(ppc_load(&C, t, b, off, 2, 1), "lha %d, %d(%d)", t, off, b);
        V(ppc_load(&C, t, b, off, 4, 0), "lwz %d, %d(%d)", t, off, b);
        V(ppc_store(&C, t, b, off, 1), "stb %d, %d(%d)", t, off, b);
        V(ppc_store(&C, t, b, off, 2), "sth %d, %d(%d)", t, off, b);
        V(ppc_store(&C, t, b, off, 4), "stw %d, %d(%d)", t, off, b);
        V(ppc_stwu(&C, t, b, off), "stwu %d, %d(%d)", t, off, b);
        V(ppc_loadx(&C, t, b, x, 1, 0), "lbzx %d, %d, %d", t, b, x);
        V(ppc_loadx(&C, t, b, x, 2, 0), "lhzx %d, %d, %d", t, b, x);
        V(ppc_loadx(&C, t, b, x, 2, 1), "lhax %d, %d, %d", t, b, x);
        V(ppc_loadx(&C, t, b, x, 4, 0), "lwzx %d, %d, %d", t, b, x);
        V(ppc_storex(&C, t, b, x, 1), "stbx %d, %d, %d", t, b, x);
        V(ppc_storex(&C, t, b, x, 2), "sthx %d, %d, %d", t, b, x);
        V(ppc_storex(&C, t, b, x, 4), "stwx %d, %d, %d", t, b, x);
        V(ppc_stwux(&C, t, b, x), "stwux %d, %d, %d", t, b, x);
        V(ppc_lbrx(&C, t, R[(j + 6) % NR], x, 2), "lhbrx %d, %d, %d", t,
          R[(j + 6) % NR], x);
        V(ppc_lbrx(&C, t, R[(j + 6) % NR], x, 4), "lwbrx %d, %d, %d", t,
          R[(j + 6) % NR], x);
        V(ppc_stbrx(&C, t, R[(j + 6) % NR], x, 2), "sthbrx %d, %d, %d", t,
          R[(j + 6) % NR], x);
        V(ppc_stbrx(&C, t, R[(j + 6) % NR], x, 4), "stwbrx %d, %d, %d", t,
          R[(j + 6) % NR], x);
        V(ppc_lwarx(&C, t, R[(j + 6) % NR], x), "lwarx %d, %d, %d", t,
          R[(j + 6) % NR], x);
        V(ppc_stwcx(&C, t, R[(j + 6) % NR], x), "stwcx. %d, %d, %d", t,
          R[(j + 6) % NR], x);
    }
    /* branches, at the ends of their reach and with every condition and
     * CR field */
    {
        static const long boff[] = { -32768, -32764, -4, 0, 4, 8, 32764 };
        static const long loff[] = { -33554432L, -4, 0, 4, 33554428L };
        for (k = 0; k < 6; k++)
            for (j = 0; j < (int)(sizeof boff / sizeof boff[0]); j++) {
                int cr = (k + j) % 8;
                V(ppc_w(&C, ppc_enc_bc(k, cr, boff[j])), "b%s %d, (%ld)",
                  cname[k], cr, boff[j]);
            }
        for (j = 0; j < (int)(sizeof boff / sizeof boff[0]); j++)
            V(ppc_w(&C, ppc_enc_bdnz(boff[j])), "bdnz (%ld)", boff[j]);
        for (j = 0; j < (int)(sizeof loff / sizeof loff[0]); j++) {
            V(ppc_w(&C, ppc_enc_b(loff[j], 0)), "b (%ld)", loff[j]);
            V(ppc_w(&C, ppc_enc_b(loff[j], 1)), "bl (%ld)", loff[j]);
        }
    }
    V(ppc_blr(&C), "blr");
    V(ppc_bctr(&C), "bctr");
    V(ppc_bctrl(&C), "bctrl");
    V(ppc_nop(&C), "nop");
    V(ppc_sync(&C), "sync");
    V(ppc_isync(&C), "isync");
    V(ppc_trap(&C), "trap");
    V(ppc_sc(&C), "sc");
    V(ppc_tlbwe(&C), "tlbwe");
    V(ppc_wrteei(&C, 0), "wrteei 0");
    V(ppc_wrteei(&C, 1), "wrteei 1");
    for (j = 0; j < NR; j++) {
        static const int sprs[] = { 1, 8, 9, 26, 27, 48, 62, 63, 268, 272,
                                    400, 415, 624, 625, 626, 627, 944, 1008,
                                    1023, 22 };
        int r = R[j], spr = sprs[j % 20];
        V(ppc_mfspr(&C, r, spr), "mfspr %d, %d", r, spr);
        V(ppc_mtspr(&C, spr, r), "mtspr %d, %d", spr, r);
        V(ppc_mflr(&C, r), "mflr %d", r);
        V(ppc_mtlr(&C, r), "mtlr %d", r);
        V(ppc_mtctr(&C, r), "mtctr %d", r);
        V(ppc_mfmsr(&C, r), "mfmsr %d", r);
        V(ppc_mtmsr(&C, r), "mtmsr %d", r);
    }
}

/* ---- ppc_li, executed ------------------------------------------------- */

/* addi rd, 0, imm; addis rd, 0, imm; ori rd, rd, imm -- the three ppc_li
 * may emit, evaluated. Anything else is a failure. */
static int run_li(const struct code *c, int rd, unsigned long *out)
{
    unsigned long reg[32] = { 0 };
    for (int p = 0; p + 4 <= c->len; p += 4) {
        unsigned long w = ppc_get_word(c->p + p);
        unsigned op = (unsigned)(w >> 26), rt = (unsigned)(w >> 21) & 31,
                 ra = (unsigned)(w >> 16) & 31, imm = (unsigned)w & 0xffff;
        unsigned long simm = imm & 0x8000 ? (0xffff0000UL | imm) : imm;
        unsigned long base = ra ? reg[ra] : 0;
        switch (op) {
        case 14: reg[rt] = (base + simm) & 0xffffffffUL; break;          /* addi */
        case 15: reg[rt] = (base + (simm << 16)) & 0xffffffffUL; break;  /* addis */
        case 24: reg[ra] = reg[rt] | imm; break;                         /* ori */
        default:
            printf("li: unexpected instruction 0x%08lx\n", w);
            return 0;
        }
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
        ppc_li(&c, 9, v);
        if (!run_li(&c, 9, &got))
            return 1;
        if (got != ((unsigned long)v & 0xffffffffUL)) {
            printf("li 0x%llx computed 0x%lx\n", (unsigned long long)v, got);
            return 1;
        }
        if (c.len != ppc_li_len(v)) {
            printf("li 0x%llx is %d bytes, ppc_li_len said %d\n",
                   (unsigned long long)v, c.len, ppc_li_len(v));
            return 1;
        }
        free(c.p);
        n++;
    }
    printf("%d ppc_li sequences each compute the value asked for\n", n);
    return 0;
}

/* ---- the refusals ----------------------------------------------------- */

#define NREFUSE 20
static void refuse(int n)
{
    struct code c = { 0 };
    switch (n) {
    case 0:  ppc_imm(&c, PPC_ADDI, 3, 4, 32768); break;
    case 1:  ppc_imm(&c, PPC_ADDI, 3, 4, -32769); break;
    case 2:  ppc_imm(&c, PPC_ADDI, 3, 0, 1); break;          /* r0 reads 0 */
    case 3:  ppc_imm(&c, PPC_ORI, 3, 4, -1); break;
    case 4:  ppc_imm(&c, PPC_ANDI_, 3, 4, 65536); break;
    case 5:  ppc_load(&c, 3, 1, 32768, 4, 0); break;
    case 6:  ppc_store(&c, 3, 1, -32769, 4); break;
    case 7:  ppc_load(&c, 3, 0, 0, 4, 0); break;             /* base r0 */
    case 8:  ppc_load(&c, 3, 1, 0, 1, 1); break;             /* no lba */
    case 9:  ppc_srawi(&c, 3, 4, 32); break;
    case 10: ppc_w(&c, ppc_enc_bc(PPC_EQ, 0, 32768)); break;
    case 11: ppc_w(&c, ppc_enc_bc(PPC_EQ, 0, -32772)); break;
    case 12: ppc_w(&c, ppc_enc_bc(PPC_EQ, 0, 6)); break;
    case 13: ppc_w(&c, ppc_enc_b(1L << 25, 0)); break;
    case 14: ppc_rlwinm(&c, 3, 4, 32, 0, 31); break;
    case 15: ppc_cmpi(&c, 0, 0, 3, -1); break;               /* cmplwi */
    case 16: ppc_cmpi(&c, 8, 1, 3, 0); break;
    case 17: ppc_alu(&c, PPC_ADD, 32, 3, 4); break;
    case 18: ppc_mtspr(&c, 1024, 3); break;
    case 19: ppc_store(&c, 3, 1, 0, 3); break;
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
    fprintf(stderr, "usage: ppccheck --vocab | --li | --refuse N|list\n");
    return 2;
}
