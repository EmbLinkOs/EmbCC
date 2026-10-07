/* Hands EmbCC's whole Xtensa vocabulary to a referee and checks each
 * encoding, one instruction at a time.
 *
 *   xtensacheck --vocab BASE   one line per form: the instruction as
 *                              QEMU's disassembler prints it when the
 *                              form sits at address BASE + its offset, a
 *                              '|', and the bytes src/arch/xtensa/emit.c
 *                              produced for it
 *   xtensacheck --li           executes every xt_li_inline and
 *                              xt_addi_any sequence in a small
 *                              interpreter and checks the value and the
 *                              length promised
 *   xtensacheck --refuse N     provokes encoder range check N, which must
 *                              stop the process with an internal error;
 *                              `--refuse list` prints how many there are
 *
 * There is no Xtensa assembler on the machines this is tested on (no
 * llvm-mc target, no binutils), so the referee is the other direction:
 * tests/golden/xtensa-encoding.sh writes the bytes into memory of QEMU's
 * de212 core, stopped before it runs, and has QEMU's monitor disassemble
 * them (`xp/Ni`). That disassembler is generated from the core's own ISA
 * description, and the expected text below is written from the ISA
 * manual -- the operand as the instruction means it (a shift count, a
 * byte offset, a branch's absolute target), never the field -- so a field
 * in the wrong place, a wrong scale or a wrong bias reads back as a
 * different line. An instruction the de212 does not have prints as
 * something else (or "???"), which is also a mismatch.
 *
 * The text and the bytes of a line come from ONE call, so a form printed
 * but not encoded cannot shift every comparison after it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/xtensa/emit.h"

static struct code C;
static unsigned long BASE;

/* The absolute address of the next instruction. */
#define PC ((unsigned long)(BASE + (unsigned long)C.len))

/* Encode, then print the text and the bytes just written. Each entry is
 * exactly one instruction. */
#define V(call, ...) do {                                                   \
        int at_ = C.len;                                                    \
        char txt_[160];                                                     \
        snprintf(txt_, sizeof txt_, __VA_ARGS__);                           \
        call;                                                               \
        if (C.len - at_ != 3) {                                             \
            fprintf(stderr, "xtensacheck: an entry emitted %d bytes\n",     \
                    C.len - at_);                                           \
            exit(2);                                                        \
        }                                                                   \
        printf("%s|%02x%02x%02x\n", txt_, C.p[at_], C.p[at_ + 1],           \
               C.p[at_ + 2]);                                               \
    } while (0)

/* The registers each field is swept over: all sixteen. */
#define NR 16

static void vocab(void)
{
    static const struct { int op; const char *nm; } alu[] = {
        { XT_ADD, "add" }, { XT_SUB, "sub" }, { XT_AND, "and" },
        { XT_OR, "or" }, { XT_XOR, "xor" },
        { XT_ADDX2, "addx2" }, { XT_ADDX4, "addx4" }, { XT_ADDX8, "addx8" },
        { XT_SUBX2, "subx2" }, { XT_SUBX4, "subx4" }, { XT_SUBX8, "subx8" },
        { XT_MULL, "mull" }, { XT_MUL16U, "mul16u" }, { XT_MUL16S, "mul16s" },
        { XT_QUOS, "quos" }, { XT_QUOU, "quou" }, { XT_REMS, "rems" },
        { XT_REMU, "remu" }, { XT_MIN, "min" }, { XT_MAX, "max" },
        { XT_MINU, "minu" }, { XT_MAXU, "maxu" }, { XT_MOVEQZ, "moveqz" },
        { XT_MOVNEZ, "movnez" }, { XT_MOVLTZ, "movltz" },
        { XT_MOVGEZ, "movgez" }, { XT_SRC, "src" }
    };
    static const long movis[] = { -2048, -2047, -256, -129, -128, -1, 0, 1,
                                  127, 128, 255, 256, 1024, 2046, 2047 };
    static const long addis[] = { -128, -127, -64, -1, 0, 1, 64, 126, 127 };
    static const long addmis[] = { -32768, -32512, -256, 0, 256, 512,
                                   32256, 32512 };
    static const int shl[] = { 1, 2, 7, 8, 15, 16, 17, 30, 31 };
    static const int shr[] = { 0, 1, 2, 7, 8, 14, 15 };
    static const int sra[] = { 0, 1, 15, 16, 17, 30, 31 };
    static const int ext[][2] = { { 0, 1 }, { 0, 16 }, { 1, 8 }, { 15, 16 },
                                  { 16, 16 }, { 17, 3 }, { 24, 8 },
                                  { 31, 1 }, { 30, 2 }, { 8, 12 } };
    static const int bits[] = { 7, 8, 14, 15, 16, 21, 22 };
    static const long offs1[] = { 0, 1, 2, 3, 127, 128, 254, 255 };
    static const long offs2[] = { 0, 2, 4, 254, 256, 508, 510 };
    static const long offs4[] = { 0, 4, 8, 252, 256, 512, 1016, 1020 };
    static const long eoffs[] = { -64, -60, -32, -16, -12, -8, -4 };
    static const long boffs[] = { -128, -127, -64, -4, -3, 0, 1, 3, 64,
                                  126, 127 };
    static const long bzoffs[] = { -2048, -2047, -1024, -129, -128, -3, 0, 3,
                                   127, 128, 1024, 2046, 2047 };
    static const long joffs[] = { -131072, -131071, -65536, -2049, -2048,
                                  -129, -128, -4, -3, 0, 3, 128, 2048,
                                  65536, 131070, 131071 };
    static const long long b4c[] = { -1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16,
                                     32, 64, 128, 256 };
    static const long long b4cu[] = { 32768, 65536, 2, 3, 4, 5, 6, 7, 8, 10,
                                      12, 16, 32, 64, 128, 256 };
    static const struct { int c; const char *nm; } bc[] = {
        { XT_BNONE, "bnone" }, { XT_BEQ, "beq" }, { XT_BLT, "blt" },
        { XT_BLTU, "bltu" }, { XT_BALL, "ball" }, { XT_BBC, "bbc" },
        { XT_BANY, "bany" }, { XT_BNE, "bne" }, { XT_BGE, "bge" },
        { XT_BGEU, "bgeu" }, { XT_BNALL, "bnall" }, { XT_BBS, "bbs" }
    };
    static const char *const zc[] = { "beqz", "bnez", "bltz", "bgez" };
    static const char *const ic[] = { "beqi", "bnei", "blti", "bgei",
                                      "bltui", "bgeui" };
    static const struct { int sr; const char *nm; } srs[] = {
        { XT_SR_SAR, "sar" }, { XT_SR_SCOMPARE1, "scompare1" },
        { XT_SR_WINDOWBASE, "windowbase" },
        { XT_SR_WINDOWSTART, "windowstart" }, { XT_SR_EPC1, "epc1" },
        { XT_SR_DEPC, "depc" }, { XT_SR_EXCSAVE1, "excsave1" },
        { XT_SR_INTENABLE, "intenable" }, { XT_SR_PS, "ps" },
        { XT_SR_VECBASE, "vecbase" }, { XT_SR_EXCCAUSE, "exccause" },
        { XT_SR_CCOUNT, "ccount" }, { XT_SR_EXCVADDR, "excvaddr" }
    };
    int k, j;

    for (k = 0; k < (int)(sizeof alu / sizeof alu[0]); k++)
        for (j = 0; j < NR; j++) {
            int r = j, s = (j + 5) % NR, t = (j + 11) % NR;
            V(xt_alu(&C, alu[k].op, r, s, t), "%s\ta%d, a%d, a%d",
              alu[k].nm, r, s, t);
        }
    for (j = 0; j < NR; j++) {
        int r = j, s = (j + 3) % NR;
        V(xt_mov(&C, r, s), "or\ta%d, a%d, a%d", r, s, s);
        V(xt_neg(&C, r, s), "neg\ta%d, a%d", r, s);
        V(xt_abs(&C, r, s), "abs\ta%d, a%d", r, s);
        V(xt_nsa(&C, r, s), "nsa\ta%d, a%d", r, s);
        V(xt_nsau(&C, r, s), "nsau\ta%d, a%d", r, s);
        V(xt_sll(&C, r, s), "sll\ta%d, a%d", r, s);
        V(xt_srl(&C, r, s), "srl\ta%d, a%d", r, s);
        V(xt_sra(&C, r, s), "sra\ta%d, a%d", r, s);
        V(xt_ssl(&C, s), "ssl\ta%d", s);
        V(xt_ssr(&C, s), "ssr\ta%d", s);
        V(xt_ssa8l(&C, s), "ssa8l\ta%d", s);
        V(xt_callx(&C, 0, s), "callx0\ta%d", s);
        V(xt_callx(&C, 1, s), "callx4\ta%d", s);
        V(xt_callx(&C, 2, s), "callx8\ta%d", s);
        V(xt_callx(&C, 3, s), "callx12\ta%d", s);
        V(xt_jx(&C, s), "jx\ta%d", s);
        V(xt_movsp(&C, r, s), "movsp\ta%d, a%d", r, s);
    }
    for (j = 0; j < (int)(sizeof movis / sizeof movis[0]); j++)
        V(xt_movi(&C, j % NR, movis[j]), "movi\ta%d, %ld", j % NR, movis[j]);
    for (j = 0; j < NR; j++) {
        V(xt_movi(&C, j, (long)j * 131 - 1000), "movi\ta%d, %ld", j,
          (long)j * 131 - 1000);
        V(xt_addi(&C, j, (j + 7) % NR, addis[j % 9]), "addi\ta%d, a%d, %ld",
          j, (j + 7) % NR, addis[j % 9]);
        V(xt_addmi(&C, (j + 2) % NR, j, addmis[j % 8]),
          "addmi\ta%d, a%d, %ld", (j + 2) % NR, j, addmis[j % 8]);
    }
    for (j = 0; j < (int)(sizeof shl / sizeof shl[0]); j++) {
        int r = (j * 3) % NR, s = (j * 5 + 1) % NR;
        V(xt_slli(&C, r, s, shl[j]), "slli\ta%d, a%d, %d", r, s, shl[j]);
    }
    for (j = 0; j < (int)(sizeof shr / sizeof shr[0]); j++) {
        int r = (j * 7 + 2) % NR, t = (j * 3 + 9) % NR;
        V(xt_srli(&C, r, t, shr[j]), "srli\ta%d, a%d, %d", r, t, shr[j]);
    }
    for (j = 0; j < (int)(sizeof sra / sizeof sra[0]); j++) {
        int r = (j * 5 + 4) % NR, t = (j * 11 + 1) % NR;
        V(xt_srai(&C, r, t, sra[j]), "srai\ta%d, a%d, %d", r, t, sra[j]);
    }
    for (j = 0; j < (int)(sizeof ext / sizeof ext[0]); j++) {
        int r = (j * 3 + 1) % NR, t = (j * 7 + 2) % NR;
        V(xt_extui(&C, r, t, ext[j][0], ext[j][1]), "extui\ta%d, a%d, %d, %d",
          r, t, ext[j][0], ext[j][1]);
    }
    for (j = 0; j < 32; j++)
        V(xt_ssai(&C, j), "ssai\t%d", j);
    for (j = 0; j < (int)(sizeof bits / sizeof bits[0]); j++) {
        int r = (j * 3 + 5) % NR, s = (j * 5 + 2) % NR;
        V(xt_sext(&C, r, s, bits[j]), "sext\ta%d, a%d, %d", r, s, bits[j]);
        V(xt_clamps(&C, s, r, bits[j]), "clamps\ta%d, a%d, %d", s, r, bits[j]);
    }
    for (j = 0; j < (int)(sizeof offs1 / sizeof offs1[0]); j++) {
        int t = (j * 3) % NR, b = (j * 5 + 1) % NR;
        V(xt_load(&C, t, b, offs1[j], 1, 0), "l8ui\ta%d, a%d, %ld", t, b,
          offs1[j]);
        V(xt_store(&C, b, t, offs1[j], 1), "s8i\ta%d, a%d, %ld", b, t,
          offs1[j]);
    }
    for (j = 0; j < (int)(sizeof offs2 / sizeof offs2[0]); j++) {
        int t = (j * 3 + 2) % NR, b = (j * 7 + 1) % NR;
        V(xt_load(&C, t, b, offs2[j], 2, 0), "l16ui\ta%d, a%d, %ld", t, b,
          offs2[j]);
        V(xt_load(&C, b, t, offs2[j], 2, 1), "l16si\ta%d, a%d, %ld", b, t,
          offs2[j]);
        V(xt_store(&C, t, b, offs2[j], 2), "s16i\ta%d, a%d, %ld", t, b,
          offs2[j]);
    }
    for (j = 0; j < (int)(sizeof offs4 / sizeof offs4[0]); j++) {
        int t = (j * 3 + 4) % NR, b = (j * 5 + 6) % NR;
        V(xt_load(&C, t, b, offs4[j], 4, 0), "l32i\ta%d, a%d, %ld", t, b,
          offs4[j]);
        V(xt_store(&C, b, t, offs4[j], 4), "s32i\ta%d, a%d, %ld", b, t,
          offs4[j]);
        V(xt_l32ai(&C, t, b, offs4[j]), "l32ai\ta%d, a%d, %ld", t, b,
          offs4[j]);
        V(xt_s32ri(&C, b, t, offs4[j]), "s32ri\ta%d, a%d, %ld", b, t,
          offs4[j]);
        V(xt_s32c1i(&C, t, b, offs4[j]), "s32c1i\ta%d, a%d, %ld", t, b,
          offs4[j]);
    }
    for (j = 0; j < NR; j++) {
        int t = j, b = (j + 9) % NR;
        long o = eoffs[j % 7];
        V(xt_l32e(&C, t, b, o), "l32e\ta%d, a%d, %ld", t, b, o);
        V(xt_s32e(&C, b, t, o), "s32e\ta%d, a%d, %ld", b, t, o);
    }
    /* l32r back to literals at a spread of distances: the literal is at
     * an absolute address the disassembler prints. Each l32r's own
     * address modulo 4 varies as the vocabulary grows by threes, so the
     * (at + 3) & ~3 rounding is exercised at every phase. */
    for (j = 0; j < 24; j++) {
        static const long back[] = { 4, 8, 12, 64, 1024, 4096, 65536,
                                     131072, 262140, 262144 };
        long at = C.len;
        long base4 = (at + 3) & ~3L;
        long lit = base4 - back[j % 10];
        int t = j % NR;
        V(xt_l32r(&C, t, lit), "l32r\ta%d, 0x%lx", t,
          (unsigned long)(BASE + (unsigned long)lit));
    }
    for (k = 0; k < (int)(sizeof bc / sizeof bc[0]); k++)
        for (j = 0; j < (int)(sizeof boffs / sizeof boffs[0]); j++) {
            int s = (j * 3 + k) % NR, t = (j * 5 + k + 1) % NR;
            long o = boffs[j];
            V(xt_w(&C, xt_enc_b(bc[k].c, s, t, o)), "%s\ta%d, a%d, 0x%lx",
              bc[k].nm, s, t, PC + 4 + (unsigned long)o);
        }
    for (j = 0; j < 32; j++) {
        int s = j % NR;
        long o = boffs[j % 11];
        V(xt_w(&C, xt_enc_bbi(0, s, j, o)), "bbci\ta%d, %d, 0x%lx", s, j,
          PC + 4 + (unsigned long)o);
        V(xt_w(&C, xt_enc_bbi(1, s, j, o)), "bbsi\ta%d, %d, 0x%lx", s, j,
          PC + 4 + (unsigned long)o);
    }
    for (k = 0; k < 4; k++)
        for (j = 0; j < (int)(sizeof bzoffs / sizeof bzoffs[0]); j++) {
            int s = (j * 3 + k * 5) % NR;
            long o = bzoffs[j];
            V(xt_w(&C, xt_enc_bz(k, s, o)), "%s\ta%d, 0x%lx", zc[k], s,
              PC + 4 + (unsigned long)o);
        }
    for (k = 0; k < 6; k++)
        for (j = 0; j < 16; j++) {
            int s = (j + k * 3) % NR;
            long long kk = k >= XT_BLTUI ? b4cu[j] : b4c[j];
            long o = boffs[(j + k) % 11];
            V(xt_w(&C, xt_enc_bi(k, s, kk, o)), "%s\ta%d, %lld, 0x%lx",
              ic[k], s, kk, PC + 4 + (unsigned long)o);
        }
    for (j = 0; j < (int)(sizeof joffs / sizeof joffs[0]); j++) {
        long o = joffs[j];
        V(xt_w(&C, xt_enc_j(o)), "j\t0x%lx", PC + 4 + (unsigned long)o);
    }
    /* calls to targets at a spread of distances, both directions; the
     * target is 4-aligned and the call at every phase mod 4 */
    for (j = 0; j < 16; j++) {
        static const long dist[] = { 0, 4, 8, 1024, 262144, 524284,
                                     -4, -8, -1024, -262144, -524288 };
        int n = j % 4;
        long at = C.len;
        long target = ((at & ~3L) + 4) + dist[j % 11];
        static const char *const cn[] = { "call0", "call4", "call8",
                                          "call12" };
        V(xt_w(&C, xt_enc_call_to(n, at, target)), "%s\t0x%lx", cn[n],
          (unsigned long)(BASE + (unsigned long)target));
    }
    for (j = 0; j < NR; j++) {
        static const long fr[] = { 0, 8, 16, 32, 48, 256, 4096, 32760 };
        V(xt_entry(&C, j, fr[j % 8]), "entry\ta%d, %ld", j, fr[j % 8]);
    }
    for (j = -8; j < 8; j++)
        V(xt_rotw(&C, j), "rotw\t%d", j);
    for (k = 0; k < (int)(sizeof srs / sizeof srs[0]); k++) {
        int t = (k * 5 + 3) % NR;
        V(xt_rsr(&C, t, srs[k].sr), "rsr.%s\ta%d", srs[k].nm, t);
        V(xt_wsr(&C, t, srs[k].sr), "wsr.%s\ta%d", srs[k].nm, t);
        V(xt_xsr(&C, t, srs[k].sr), "xsr.%s\ta%d", srs[k].nm, t);
    }
    V(xt_ret(&C), "ret");
    V(xt_retw(&C), "retw");
    V(xt_nop(&C), "nop");
    V(xt_ill(&C), "ill");
    V(xt_isync(&C), "isync");
    V(xt_rsync(&C), "rsync");
    V(xt_esync(&C), "esync");
    V(xt_dsync(&C), "dsync");
    V(xt_memw(&C), "memw");
    V(xt_extw(&C), "extw");
    V(xt_rfe(&C), "rfe");
    V(xt_rfde(&C), "rfde");
    V(xt_rfwo(&C), "rfwo");
    V(xt_rfwu(&C), "rfwu");
    V(xt_syscall(&C), "syscall");
    V(xt_simcall(&C), "simcall");
    for (j = 0; j < 16; j++)
        V(xt_break(&C, j, 15 - j), "break\t%d, %d", j, 15 - j);
    for (j = 1; j <= 6; j++)
        V(xt_rfi(&C, j), "rfi\t%d", j);
    for (j = 0; j < 16; j++)
        V(xt_rsil(&C, j, j % 6), "rsil\ta%d, %d", j, j % 6);
    for (j = 0; j < 6; j++)
        V(xt_waiti(&C, j), "waiti\t%d", j);
}

/* ---- xt_li_inline and xt_addi_any, executed ------------------------------ */

/* The instructions the two may emit, evaluated: movi, slli, addi, addmi.
 * Anything else fails, because then the sequence is not the one this mode
 * understands. */
static int run(const struct code *c, unsigned long *reg)
{
    for (int p = 0; p < c->len; p += 3) {
        unsigned long w = xt_get(c, p);
        int op0 = (int)(w & 15), t = (int)(w >> 4) & 15,
            s = (int)(w >> 8) & 15, r = (int)(w >> 12) & 15,
            op1 = (int)(w >> 16) & 15, op2 = (int)(w >> 20) & 15;
        unsigned imm8 = (unsigned)(w >> 16) & 0xff;
        long simm8 = imm8 & 0x80 ? (long)imm8 - 256 : (long)imm8;
        if (op0 == 2 && r == 10) {                               /* movi */
            unsigned long f = ((unsigned long)s << 8) | imm8;
            long v = f & 0x800 ? (long)f - 4096 : (long)f;
            reg[t] = (unsigned long)v & 0xffffffffUL;
        } else if (op0 == 2 && r == 12) {                        /* addi */
            reg[t] = (reg[s] + (unsigned long)simm8) & 0xffffffffUL;
        } else if (op0 == 2 && r == 13) {                        /* addmi */
            reg[t] = (reg[s] + ((unsigned long)simm8 << 8)) & 0xffffffffUL;
        } else if (op0 == 0 && op1 == 1 && (op2 == 0 || op2 == 1)) {
            int sa = 32 - ((op2 << 4) | t);                     /* slli */
            reg[r] = (reg[s] << sa) & 0xffffffffUL;
        } else {
            printf("li: unexpected instruction 0x%06lx\n", w);
            return 0;
        }
    }
    return 1;
}

static int check_li(void)
{
    static const long long fixed[] = {
        0, 1, -1, 2047, 2048, -2048, -2049, 4096, 65535, 65536, 0x10000,
        0x7ff00000LL, 0x80000000LL, 0xfff00000LL, 0x12345678LL, 0x7fffffffLL,
        -2147483647LL - 1, 0xffffffffLL, 0x60800000LL, 0x3ffe0000LL,
        0x40000000LL, 255 << 8, (2047LL << 20) + 127, (2047LL << 20) - 128,
        -524288LL - 128, 0x40080000LL
    };
    int n = 0, inl = 0, na = 0;
    unsigned long s = 12345;
    for (int k = 0; k < (int)(sizeof fixed / sizeof fixed[0]) + 20000; k++) {
        long long v;
        struct code c = { 0 };
        unsigned long reg[16] = { 0 };
        if (k < (int)(sizeof fixed / sizeof fixed[0])) {
            v = fixed[k];
        } else {
            s = s * 1103515245UL + 12345UL;
            v = (long long)(s & 0xffffffffUL);
            switch (k & 3) {
            case 0: v >>= (k >> 2) % 32; break;
            case 1: v = (long long)(int)(unsigned)v >> ((k >> 2) % 24); break;
            case 2: v &= ~0xfffLL; v = (long long)(int)(unsigned)v; break;
            default: break;
            }
        }
        if (xt_li_inline(&c, 9, v)) {
            if (!run(&c, reg))
                return 1;
            if (reg[9] != ((unsigned long)v & 0xffffffffUL)) {
                printf("li 0x%llx computed 0x%lx\n", (unsigned long long)v,
                       reg[9]);
                return 1;
            }
            inl++;
        } else {
            na++;
        }
        if (c.len != xt_li_inline_len(v)) {
            printf("li 0x%llx is %d bytes, xt_li_inline_len said %d\n",
                   (unsigned long long)v, c.len, xt_li_inline_len(v));
            return 1;
        }
        free(c.p);
        n++;
    }
    for (long long imm = -40000; imm <= 40000; imm += 7) {
        struct code c = { 0 };
        unsigned long reg[16] = { 0 };
        reg[3] = 0x10000000UL;
        if (xt_addi_any(&c, 4, 3, imm)) {
            if (!run(&c, reg))
                return 1;
            if (reg[4] != ((0x10000000UL + (unsigned long)imm) & 0xffffffffUL)) {
                printf("addi_any %lld computed 0x%lx\n", imm, reg[4]);
                return 1;
            }
        }
        if (c.len != xt_addi_any_len(imm)) {
            printf("addi_any %lld is %d bytes, xt_addi_any_len said %d\n",
                   imm, c.len, xt_addi_any_len(imm));
            return 1;
        }
        free(c.p);
    }
    printf("%d constants: %d built inline, each computing the value asked "
           "for, %d left to a literal; xt_addi_any checked over "
           "-40000..40000\n", n, inl, na);
    return 0;
}

/* ---- the refusals ------------------------------------------------------- */

/* Every range check is load-bearing: the field is narrower than the C
 * type the caller passes, and a truncated value is a real instruction
 * that does something else. */
#define NREFUSE 40
static void refuse(int n)
{
    struct code c = { 0 };
    switch (n) {
    case 0:  xt_movi(&c, 2, 2048); break;
    case 1:  xt_movi(&c, 2, -2049); break;
    case 2:  xt_addi(&c, 2, 3, 128); break;
    case 3:  xt_addi(&c, 2, 3, -129); break;
    case 4:  xt_addmi(&c, 2, 3, 32768); break;
    case 5:  xt_addmi(&c, 2, 3, 300); break;
    case 6:  xt_slli(&c, 2, 3, 0); break;
    case 7:  xt_slli(&c, 2, 3, 32); break;
    case 8:  xt_srli(&c, 2, 3, 16); break;
    case 9:  xt_srai(&c, 2, 3, 32); break;
    case 10: xt_extui(&c, 2, 3, 0, 17); break;
    case 11: xt_extui(&c, 2, 3, 32, 1); break;
    case 12: xt_load(&c, 2, 3, 256, 1, 0); break;
    case 13: xt_load(&c, 2, 3, 3, 2, 0); break;
    case 14: xt_load(&c, 2, 3, 1024, 4, 0); break;
    case 15: xt_load(&c, 2, 3, -4, 4, 0); break;
    case 16: xt_load(&c, 2, 3, 0, 1, 1); break;
    case 17: xt_store(&c, 2, 3, 2, 4); break;
    case 18: xt_load(&c, 2, 3, 0, 8, 0); break;
    case 19: xt_w(&c, xt_enc_b(XT_BEQ, 2, 3, 128)); break;
    case 20: xt_w(&c, xt_enc_b(XT_BNE, 2, 3, -129)); break;
    case 21: xt_w(&c, xt_enc_bz(XT_BEQZ, 2, 2048)); break;
    case 22: xt_w(&c, xt_enc_bi(XT_BEQI, 2, 9, 0)); break;
    case 23: xt_w(&c, xt_enc_bi(XT_BLTUI, 2, -1, 0)); break;
    case 24: xt_w(&c, xt_enc_j(131072)); break;
    case 25: xt_w(&c, xt_enc_l32r(2, 100, 100)); break;
    case 26: xt_w(&c, xt_enc_l32r(2, 300000, 100)); break;
    case 27: xt_w(&c, xt_enc_l32r(2, 100, 50)); break;
    case 28: xt_entry(&c, 1, 32768); break;
    case 29: xt_entry(&c, 1, 36); break;
    case 30: xt_sext(&c, 2, 3, 23); break;
    case 31: xt_sext(&c, 2, 3, 6); break;
    case 32: xt_alu(&c, XT_ADD, 16, 2, 3); break;
    case 33: xt_l32e(&c, 2, 3, 0); break;
    case 34: xt_l32e(&c, 2, 3, -68); break;
    case 35: xt_w(&c, xt_enc_call_to(2, 0, 6)); break;
    case 36: xt_w(&c, xt_enc_call_to(2, 0, 524292)); break;
    case 37: xt_rotw(&c, 8); break;
    case 38: xt_w(&c, xt_enc_bbi(0, 2, 32, 0)); break;
    case 39: xt_rsr(&c, 2, 256); break;
    default: printf("no refusal %d\n", n); exit(2);
    }
    printf("refusal %d did not fire; %d bytes were emitted\n", n, c.len);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--vocab")) {
        BASE = strtoul(argv[2], NULL, 0);
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
        refuse(argc > 2 ? atoi(argv[2]) : -1);
    }
    fprintf(stderr, "usage: xtensacheck --vocab BASE | --li | "
                    "--refuse N|list\n");
    return 2;
}
