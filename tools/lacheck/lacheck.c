/* Hands EmbCC's whole LoongArch64 vocabulary to llvm-mc and compares the
 * encodings, one instruction at a time.
 *
 *   lacheck --vocab     one line per form: the assembly, a '|', and the
 *                       32-bit word src/arch/loongarch/emit.c produced
 *   lacheck --li        every la_li sequence as `li.d $a0, V|words`, the
 *                       words space-separated, for llvm-mc's own li.d
 *                       expansion to be compared with
 *   lacheck --run-li    executes every la_li sequence in a small
 *                       interpreter and checks the value, and the length
 *                       la_li_len promised
 *   lacheck --refuse N  provokes encoder range check N, which must stop
 *                       the process with an internal error;
 *                       `--refuse list` prints how many there are
 *
 * The text and the word of a line come from ONE call -- each entry below
 * formats its assembly and encodes through emit.c in the same macro -- so
 * a form printed but not encoded, or the reverse, cannot shift every
 * comparison after it and blame the wrong instruction.
 *
 * The sweep puts every register number in every field it can occupy and
 * the immediates at both ends of their extension: a LoongArch mistake is a
 * valid different instruction far more often than an invalid one -- an
 * ori handed a sign-extended field, a branch with rj and rd swapped, a
 * bstrpick whose msb and lsb trade places, an alsl whose shift field
 * holds sa rather than sa - 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/loongarch/emit.h"

static struct code C;

#define V(call, ...) do {                                                   \
        int at_ = C.len;                                                    \
        char txt_[160];                                                     \
        call;                                                               \
        if (C.len - at_ != 4) {                                             \
            fprintf(stderr, "lacheck: an entry emitted %d bytes\n",         \
                    C.len - at_);                                           \
            exit(2);                                                        \
        }                                                                   \
        snprintf(txt_, sizeof txt_, __VA_ARGS__);                           \
        printf("%s|%02x%02x%02x%02x\n", txt_, C.p[at_ + 3], C.p[at_ + 2],   \
               C.p[at_ + 1], C.p[at_]);                                     \
    } while (0)

/* Every register in every field: r21 included -- the psABI reserves it,
 * the encoder has no reason to treat its number differently. */
static const int R[] = { 0, 1, 2, 3, 4, 5, 7, 10, 11, 12, 15, 16, 20, 21,
                         22, 23, 24, 27, 30, 31 };
#define NR ((int)(sizeof R / sizeof R[0]))
static const long long SI12[] = { -2048, -2047, -256, -2, -1, 0, 1, 2, 255,
                                  256, 2046, 2047 };
#define NSI12 ((int)(sizeof SI12 / sizeof SI12[0]))
static const long long UI12[] = { 0, 1, 255, 256, 2047, 2048, 4094, 4095 };
#define NUI12 ((int)(sizeof UI12 / sizeof UI12[0]))
static const long SI20[] = { -524288, -524287, -4096, -1, 0, 1, 4095,
                             524286, 524287 };
#define NSI20 ((int)(sizeof SI20 / sizeof SI20[0]))

static void vocab(void)
{
    static const struct { int op; const char *d, *w; } alu[] = {
        { LA_ADD, "add.d", "add.w" }, { LA_SUB, "sub.d", "sub.w" },
        { LA_SLT, "slt", 0 }, { LA_SLTU, "sltu", 0 },
        { LA_AND, "and", 0 }, { LA_OR, "or", 0 }, { LA_XOR, "xor", 0 },
        { LA_NOR, "nor", 0 }, { LA_ANDN, "andn", 0 }, { LA_ORN, "orn", 0 },
        { LA_SLL, "sll.d", "sll.w" }, { LA_SRL, "srl.d", "srl.w" },
        { LA_SRA, "sra.d", "sra.w" }, { LA_ROTR, "rotr.d", "rotr.w" },
        { LA_MASKEQZ, "maskeqz", 0 }, { LA_MASKNEZ, "masknez", 0 },
        { LA_MUL, "mul.d", "mul.w" }, { LA_MULH, "mulh.d", "mulh.w" },
        { LA_MULHU, "mulh.du", "mulh.wu" }, { LA_DIV, "div.d", "div.w" },
        { LA_DIVU, "div.du", "div.wu" }, { LA_MOD, "mod.d", "mod.w" },
        { LA_MODU, "mod.du", "mod.wu" }
    };
    static const struct { int op; const char *nm; int sign, w; } aimm[] = {
        { LA_ADD, "addi.d", 1, 0 }, { LA_ADD, "addi.w", 1, 1 },
        { LA_SLT, "slti", 1, 0 }, { LA_SLTU, "sltui", 1, 0 },
        { LA_AND, "andi", 0, 0 }, { LA_OR, "ori", 0, 0 },
        { LA_XOR, "xori", 0, 0 }
    };
    static const struct { int op; const char *d, *w; } shi[] = {
        { LA_SLL, "slli.d", "slli.w" }, { LA_SRL, "srli.d", "srli.w" },
        { LA_SRA, "srai.d", "srai.w" }, { LA_ROTR, "rotri.d", "rotri.w" }
    };
    static const int sas[] = { 0, 1, 2, 15, 16, 31, 32, 33, 62, 63 };
    static const int offs[] = { -2048, -4, -1, 0, 1, 4, 255, 2047 };
    static const struct { int size, sign; const char *ld, *st; } mem[] = {
        { 1, 1, "ld.b", "st.b" }, { 1, 0, "ld.bu", 0 },
        { 2, 1, "ld.h", "st.h" }, { 2, 0, "ld.hu", 0 },
        { 4, 1, "ld.w", "st.w" }, { 4, 0, "ld.wu", 0 },
        { 8, 1, "ld.d", "st.d" }
    };
    static const int bit[][2] = { { 0, 0 }, { 31, 0 }, { 15, 0 }, { 7, 0 },
                                  { 31, 31 }, { 20, 5 }, { 63, 0 },
                                  { 63, 32 }, { 47, 16 }, { 40, 40 } };
    static const struct { int cond; const char *nm; } br[] = {
        { LA_BEQ, "beq" }, { LA_BNE, "bne" }, { LA_BLT, "blt" },
        { LA_BGE, "bge" }, { LA_BLTU, "bltu" }, { LA_BGEU, "bgeu" }
    };
    static const long boffs[] = { -131072, -131068, -8, -4, 0, 4, 8, 4096,
                                  131068 };
    static const long zoffs[] = { -4194304, -4194300, -131076, -4, 0, 4,
                                  131072, 4194300 };
    static const long joffs[] = { -134217728, -134217724, -4194308, -4, 0, 4,
                                  4194304, 134217724 };
    static const long jroffs[] = { -131072, -4, 0, 4, 2044, 131068 };
    static const int lloffs[] = { -32768, -32764, -4, 0, 4, 2048, 32764 };
    static const struct { int op; const char *nm; } am[] = {
        { LA_AMSWAP, "amswap_db" }, { LA_AMADD, "amadd_db" },
        { LA_AMAND, "amand_db" }, { LA_AMOR, "amor_db" },
        { LA_AMXOR, "amxor_db" }
    };
    static const char *const pcnm[] = { "pcaddi", "pcalau12i", "pcaddu12i",
                                        "pcaddu18i" };
    static const char *const revnm[] = { "revb.2h", "revb.4h", "revb.2w",
                                         "revb.d" };
    int k, j;

    for (k = 0; k < (int)(sizeof alu / sizeof alu[0]); k++)
        for (j = 0; j < NR; j++) {
            int d = R[j], a = R[(j + 5) % NR], b = R[(j + 11) % NR];
            V(la_alu(&C, alu[k].op, d, a, b, 0), "%s $r%d, $r%d, $r%d",
              alu[k].d, d, a, b);
            if (alu[k].w)
                V(la_alu(&C, alu[k].op, d, a, b, 1), "%s $r%d, $r%d, $r%d",
                  alu[k].w, d, a, b);
        }
    for (k = 0; k < (int)(sizeof aimm / sizeof aimm[0]); k++) {
        int n = aimm[k].sign ? NSI12 : NUI12;
        for (j = 0; j < n; j++) {
            long long v = aimm[k].sign ? SI12[j] : UI12[j];
            int d = R[j % NR], s = R[(j + 7) % NR];
            V(la_alu_imm(&C, aimm[k].op, d, s, v, aimm[k].w),
              "%s $r%d, $r%d, %lld", aimm[k].nm, d, s, v);
        }
    }
    for (k = 0; k < (int)(sizeof shi / sizeof shi[0]); k++)
        for (j = 0; j < (int)(sizeof sas / sizeof sas[0]); j++) {
            int d = R[(j * 3 + k) % NR], s = R[(j * 5 + 2) % NR];
            V(la_shift_imm(&C, shi[k].op, d, s, sas[j], 0),
              "%s $r%d, $r%d, %d", shi[k].d, d, s, sas[j]);
            if (sas[j] < 32)
                V(la_shift_imm(&C, shi[k].op, d, s, sas[j], 1),
                  "%s $r%d, $r%d, %d", shi[k].w, d, s, sas[j]);
        }
    for (j = 0; j < NR; j++) {
        int d = R[j], s = R[(j + 9) % NR], t = R[(j + 4) % NR];
        V(la_mv(&C, d, s), "or $r%d, $r%d, $r0", d, s);
        V(la_ext(&C, d, s, 1), "ext.w.b $r%d, $r%d", d, s);
        V(la_ext(&C, d, s, 2), "ext.w.h $r%d, $r%d", d, s);
        for (k = 0; k < 4; k++)
            V(la_revb(&C, k, d, s), "%s $r%d, $r%d", revnm[k], d, s);
        for (k = 1; k <= 4; k++) {
            V(la_alsl(&C, d, s, t, k, 1), "alsl.d $r%d, $r%d, $r%d, %d",
              d, s, t, k);
            V(la_alsl(&C, d, s, t, k, 0), "alsl.w $r%d, $r%d, $r%d, %d",
              d, s, t, k);
        }
        V(la_jirl(&C, d, s, jroffs[j % 6]), "jirl $r%d, $r%d, %ld", d, s,
          jroffs[j % 6]);
    }
    V(la_nop(&C), "nop");
    V(la_ret(&C), "ret");
    for (k = 0; k < (int)(sizeof bit / sizeof bit[0]); k++) {
        int d = R[(k + 2) % NR], s = R[(k + 13) % NR];
        V(la_bstrpick(&C, d, s, bit[k][0], bit[k][1], 1),
          "bstrpick.d $r%d, $r%d, %d, %d", d, s, bit[k][0], bit[k][1]);
        if (bit[k][0] < 32)
            V(la_bstrpick(&C, d, s, bit[k][0], bit[k][1], 0),
              "bstrpick.w $r%d, $r%d, %d, %d", d, s, bit[k][0], bit[k][1]);
    }
    for (j = 0; j < NSI20; j++) {
        int d = R[(j * 7) % NR], s = R[(j * 3 + 1) % NR];
        V(la_lu12i(&C, d, SI20[j]), "lu12i.w $r%d, %ld", d, SI20[j]);
        V(la_lu32i(&C, d, SI20[j]), "lu32i.d $r%d, %ld", d, SI20[j]);
        for (k = 0; k < 4; k++)
            V(la_pcrel(&C, k, d, SI20[j]), "%s $r%d, %ld", pcnm[k], d,
              SI20[j]);
        V(la_lu52i(&C, d, s, (long)SI12[j % NSI12]), "lu52i.d $r%d, $r%d, %lld",
          d, s, SI12[j % NSI12]);
    }
    for (k = 0; k < (int)(sizeof mem / sizeof mem[0]); k++)
        for (j = 0; j < (int)(sizeof offs / sizeof offs[0]); j++) {
            int d = R[(j + k) % NR], b = R[(j * 3 + 4) % NR];
            V(la_load(&C, d, b, offs[j], mem[k].size, mem[k].sign),
              "%s $r%d, $r%d, %d", mem[k].ld, d, b, offs[j]);
            if (mem[k].st)
                V(la_store(&C, d, b, offs[j], mem[k].size),
                  "%s $r%d, $r%d, %d", mem[k].st, d, b, offs[j]);
        }
    for (k = 0; k < (int)(sizeof br / sizeof br[0]); k++)
        for (j = 0; j < (int)(sizeof boffs / sizeof boffs[0]); j++) {
            int a = R[(j + k + 1) % NR], b = R[(j * 3 + 8) % NR];
            V(la_w(&C, la_enc_branch(br[k].cond, a, b, boffs[j])),
              "%s $r%d, $r%d, %ld", br[k].nm, a, b, boffs[j]);
        }
    for (j = 0; j < (int)(sizeof zoffs / sizeof zoffs[0]); j++) {
        int a = R[(j * 5 + 3) % NR];
        V(la_w(&C, la_enc_branch(LA_BEQZ, a, LA_ZERO, zoffs[j])),
          "beqz $r%d, %ld", a, zoffs[j]);
        V(la_w(&C, la_enc_branch(LA_BNEZ, a, LA_ZERO, zoffs[j])),
          "bnez $r%d, %ld", a, zoffs[j]);
    }
    for (j = 0; j < (int)(sizeof joffs / sizeof joffs[0]); j++) {
        V(la_w(&C, la_enc_j(0, joffs[j])), "b %ld", joffs[j]);
        V(la_w(&C, la_enc_j(1, joffs[j])), "bl %ld", joffs[j]);
    }
    for (j = 0; j < (int)(sizeof lloffs / sizeof lloffs[0]); j++) {
        int d = R[(j + 6) % NR], b = R[(j + 1) % NR];
        V(la_ll(&C, d, b, lloffs[j], 0), "ll.w $r%d, $r%d, %d", d, b,
          lloffs[j]);
        V(la_ll(&C, d, b, lloffs[j], 1), "ll.d $r%d, $r%d, %d", d, b,
          lloffs[j]);
        V(la_sc(&C, d, b, lloffs[j], 0), "sc.w $r%d, $r%d, %d", d, b,
          lloffs[j]);
        V(la_sc(&C, d, b, lloffs[j], 1), "sc.d $r%d, $r%d, %d", d, b,
          lloffs[j]);
    }
    for (k = 0; k < (int)(sizeof am / sizeof am[0]); k++)
        for (j = 0; j < NR; j++) {
            /* rd distinct from rj and rk, which the instruction requires */
            int d = R[j], v = R[(j + 3) % NR], a = R[(j + 8) % NR];
            V(la_am(&C, am[k].op, d, v, a, 0), "%s.w $r%d, $r%d, $r%d",
              am[k].nm, d, v, a);
            V(la_am(&C, am[k].op, d, v, a, 1), "%s.d $r%d, $r%d, $r%d",
              am[k].nm, d, v, a);
        }
    {
        static const int hints[] = { 0, 1, 0x14, 0x700, 0x7fff };
        for (j = 0; j < 5; j++) {
            V(la_dbar(&C, hints[j]), "dbar %d", hints[j]);
            V(la_break(&C, hints[j]), "break %d", hints[j]);
        }
    }
}

/* ---- la_li ------------------------------------------------------------- */

static long long li_value(int k, unsigned long long *s)
{
    static const long long fixed[] = {
        0, 1, -1, 2, -2, 2047, 2048, -2048, -2049, 4095, 4096, 4097,
        0x7ff, 0x800, 0xfff, 0x1000, 0x12345678LL, 0x7fffffffLL,
        -2147483647LL - 1, 0x80000000LL, 0xffffffffLL, 0xfffff000LL,
        0xfffff800LL, 0x7ffff800LL, 0x100000000LL, 0x123456789abcdefLL,
        0x7fffffffffffffffLL, -9223372036854775807LL - 1,
        0x7ff0000000000000LL, 0x0010000000000000LL, -4503599627370496LL,
        0x000fffffffffffffLL, 0x0008000000000000LL, 0x00080000ffffffffLL,
        0xfff8000000000000LL, 0x80000000ffffffffLL, 0x3ff0000000000000LL,
        0x4000000000000000LL, 0xfffffffffffff800LL, 0x00000000fffff800LL
    };
    int nf = (int)(sizeof fixed / sizeof fixed[0]);
    if (k < nf)
        return fixed[k];
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    {
        unsigned long long v = *s;
        switch (k & 7) {          /* fields zero or all ones, in turn */
        case 1: v &= 0xffffffffULL; break;
        case 2: v |= 0xfff0000000000000ULL; break;
        case 3: v &= 0x000fffffffffffffULL; break;
        case 4: v &= 0xfff00000000fffffULL; break;
        case 5: v = (unsigned long long)(long long)(int)(unsigned)v; break;
        case 6: v &= 0xfffULL; break;
        default: break;
        }
        return (long long)v;
    }
}
#define NLI 6000

static int li_vocab(void)
{
    unsigned long long s = 12345;
    for (int k = 0; k < NLI; k++) {
        long long v = li_value(k, &s);
        struct code c = { 0 };
        la_li(&c, LA_A0, v);
        printf("li.d $a0, %lld|", v);
        for (int p = 0; p + 4 <= c.len; p += 4)
            printf("%s%02x%02x%02x%02x", p ? " " : "", c.p[p + 3], c.p[p + 2],
                   c.p[p + 1], c.p[p]);
        printf("\n");
        free(c.p);
    }
    return 0;
}

/* The five instructions la_li may emit, evaluated at 64 bits. */
static int run_li(const struct code *c, int rd, unsigned long long *out)
{
    unsigned long long reg[32] = { 0 };
    for (int p = 0; p + 4 <= c->len; p += 4) {
        unsigned long w = (unsigned long)c->p[p] |
                          ((unsigned long)c->p[p + 1] << 8) |
                          ((unsigned long)c->p[p + 2] << 16) |
                          ((unsigned long)c->p[p + 3] << 24);
        int d = (int)(w & 31), j = (int)((w >> 5) & 31);
        unsigned long long i12 = (w >> 10) & 0xfff, i20 = (w >> 5) & 0xfffff;
        unsigned long long s12 = i12 & 0x800 ? i12 | ~0xfffULL : i12;
        unsigned long long s20 = i20 & 0x80000 ? i20 | ~0xfffffULL : i20;
        switch (w & 0xffc00000UL) {
        case 0x03800000UL: reg[d] = reg[j] | i12; break;              /* ori */
        case 0x02800000UL: {                                      /* addi.w */
            unsigned long long r = (reg[j] + s12) & 0xffffffffULL;
            reg[d] = r & 0x80000000ULL ? r | 0xffffffff00000000ULL : r;
            break;
        }
        case 0x03000000UL:                                       /* lu52i.d */
            reg[d] = (reg[j] & 0x000fffffffffffffULL) | (i12 << 52);
            break;
        default:
            switch (w & 0xfe000000UL) {
            case 0x14000000UL: reg[d] = s20 << 12; break;          /* lu12i.w */
            case 0x16000000UL:                                    /* lu32i.d */
                reg[d] = (reg[d] & 0xffffffffULL) | (s20 << 32);
                break;
            default:
                printf("li: unexpected instruction 0x%08lx\n", w);
                return 0;
            }
        }
        reg[0] = 0;
    }
    *out = reg[rd];
    return 1;
}

static int check_li(void)
{
    unsigned long long s = 12345;
    int n = 0;
    for (int k = 0; k < NLI; k++) {
        long long v = li_value(k, &s);
        struct code c = { 0 };
        unsigned long long got;
        la_li(&c, LA_T3, v);
        if (!run_li(&c, LA_T3, &got))
            return 1;
        if (got != (unsigned long long)v) {
            printf("li 0x%llx computed 0x%llx\n", (unsigned long long)v, got);
            return 1;
        }
        if (c.len != la_li_len(v)) {
            printf("li 0x%llx is %d bytes, la_li_len said %d\n",
                   (unsigned long long)v, c.len, la_li_len(v));
            return 1;
        }
        free(c.p);
        n++;
    }
    printf("%d la_li sequences each compute the value asked for\n", n);
    return 0;
}

/* ---- the refusals ----------------------------------------------------- */

#define NREFUSE 26
static void refuse(int n)
{
    struct code c = { 0 };
    switch (n) {
    case 0:  la_alu_imm(&c, LA_ADD, 4, 5, 2048, 0); break;
    case 1:  la_alu_imm(&c, LA_ADD, 4, 5, -2049, 1); break;
    case 2:  la_alu_imm(&c, LA_AND, 4, 5, -1, 0); break;
    case 3:  la_alu_imm(&c, LA_OR, 4, 5, 4096, 0); break;
    case 4:  la_alu_imm(&c, LA_SLTU, 4, 5, 4095, 0); break;
    case 5:  la_alu_imm(&c, LA_XOR, 4, 5, 1, 1); break;
    case 6:  la_load(&c, 4, 3, 2048, 4, 1); break;
    case 7:  la_store(&c, 4, 3, -2049, 8); break;
    case 8:  la_shift_imm(&c, LA_SLL, 4, 5, 32, 1); break;
    case 9:  la_shift_imm(&c, LA_SRA, 4, 5, 64, 0); break;
    case 10: la_w(&c, la_enc_branch(LA_BEQ, 4, 5, 131072)); break;
    case 11: la_w(&c, la_enc_branch(LA_BNE, 4, 5, -131076)); break;
    case 12: la_w(&c, la_enc_branch(LA_BEQ, 4, 5, 6)); break;
    case 13: la_w(&c, la_enc_branch(LA_BEQZ, 4, 0, 4194304)); break;
    case 14: la_w(&c, la_enc_j(1, 134217728)); break;
    case 15: la_bstrpick(&c, 4, 5, 3, 4, 1); break;
    case 16: la_bstrpick(&c, 4, 5, 32, 0, 0); break;
    case 17: la_alsl(&c, 4, 5, 6, 5, 1); break;
    case 18: la_lu12i(&c, 4, 524288); break;
    case 19: la_lu52i(&c, 4, 4, 2048); break;
    case 20: la_ll(&c, 4, 5, 2, 0); break;
    case 21: la_sc(&c, 4, 5, 32768, 1); break;
    case 22: la_am(&c, LA_AMSWAP, 4, 4, 5, 0); break;
    case 23: la_alu(&c, LA_AND, 4, 5, 6, 1); break;
    case 24: la_alu(&c, LA_ADD, 32, 5, 6, 0); break;
    case 25: la_jirl(&c, 0, 1, 131072); break;
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
        return li_vocab();
    if (argc > 1 && !strcmp(argv[1], "--run-li"))
        return check_li();
    if (argc > 1 && !strcmp(argv[1], "--refuse")) {
        if (argc > 2 && !strcmp(argv[2], "list")) {
            printf("%d\n", NREFUSE);
            return 0;
        }
        refuse(argc > 2 ? atoi(argv[2]) : -1);
    }
    fprintf(stderr, "usage: lacheck --vocab | --li | --run-li | "
                    "--refuse N|list\n");
    return 2;
}
