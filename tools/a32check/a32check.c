/* The A32 (ARM state) vocabulary of src/arch/thumb/a32.c, swept across its
 * operands, for llvm-mc to referee.
 *
 * Every call goes through emit.h's t_* interface with t_isa_a32 set -- the
 * way the code generator and the inline-asm assembler reach the encoder --
 * so the dispatch is checked as well as the words. The bytes go to stdout;
 * the assembly each call is meant to be goes to stderr, one line per
 * instruction, with its offset and the a32_* encoder that wrote it:
 *
 *      add r0, r1, #255        @ 0x1c a32_alu_imm
 *
 * tests/golden/arm-a32-encoding.sh assembles the text with llvm-mc
 * -triple=armv7a-none-eabi and compares the two streams word for word. An
 * instruction ARMv7-A does not have is a line llvm-mc rejects; an encoding
 * that is a DIFFERENT instruction is a word that differs.
 *
 * Every call must write exactly the words its form has, so an encoder
 * that wrote nothing, or one word too many, stops the sweep here.
 *
 * Some calls are expected to write TWO instructions (a constant that
 * takes movw and movt, an addw that splits); the listing then has two
 * lines for them, the split computed here from the value, so llvm-mc
 * also checks that the pieces add up to what was asked.
 *
 * `--refuse` checks the other half of the contract: each int-returning
 * encoder answers 0 and writes nothing for an operand A32 cannot say.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/thumb/emit.h"
#include "../../src/arch/thumb/a32.h"

static struct code C;
static long n_insn;

static const char *const R[16] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
};
static const char *const COND[15] = {
    "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le", ""
};

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "a32check: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* The call that began at `at` wrote these lines (one per word, separated
 * by '\n' in `text`). */
static void say(int at, const char *enc, const char *fmt, ...)
{
    char text[512];
    va_list ap;
    int lines = 1, off = at;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    for (const char *p = text; *p; p++)
        lines += *p == '\n';
    if (C.len - at != 4 * lines)
        die("%s wrote %d bytes for `%s`, where it is %d instruction(s)", enc,
            C.len - at, text, lines);
    for (char *p = text, *e; p; p = e) {
        e = strchr(p, '\n');
        if (e)
            *e++ = 0;
        fprintf(stderr, "\t%-36s @ 0x%x %s\n", p, (unsigned)off, enc);
        off += 4;
        n_insn++;
    }
}

#define I(ENC, CALL, ...) \
    do { int at_ = C.len; (void)(CALL); say(at_, ENC, __VA_ARGS__); } while (0)

/* Under an IT queue entry: the call takes `cond` into its condition field. */
#define IC(COND_, ENC, CALL, ...) \
    do { int at_ = C.len; t_it(&C, (COND_), ""); (void)(CALL); \
         say(at_, ENC, __VA_ARGS__); } while (0)

static const char *rlist(unsigned mask)
{
    static char buf[128];
    int n = 0;
    buf[n++] = '{';
    for (int r = 0; r < 16; r++)
        if (mask & (1u << r))
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s",
                          n > 1 ? ", " : "", R[r]);
    buf[n++] = '}';
    buf[n] = 0;
    return buf;
}

/* Every value an A32 modified immediate can hold, ascending. */
static unsigned g_imm[4096];
static int g_nimm;

static int cmp_u(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return x < y ? -1 : x > y;
}

static void collect_imms(void)
{
    for (unsigned rot = 0; rot < 16; rot++)
        for (unsigned v = 0; v < 256; v++) {
            unsigned s = 2 * rot;
            unsigned x = s ? (v >> s) | (v << (32 - s)) : v;
            g_imm[g_nimm++] = x;
        }
    qsort(g_imm, (size_t)g_nimm, sizeof g_imm[0], cmp_u);
    int k = 0;
    for (int i = 0; i < g_nimm; i++)
        if (!k || g_imm[k - 1] != g_imm[i])
            g_imm[k++] = g_imm[i];
    g_nimm = k;
}

static const int SOME[] = { 0, 1, 7, 8, 12, 13, 14 };
#define NSOME ((int)(sizeof SOME / sizeof SOME[0]))
static const int GPR[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14 };
#define NGPR ((int)(sizeof GPR / sizeof GPR[0]))
/* a general register picked by k: never sp or pc, which most forms reserve */
#define G(k) GPR[(unsigned)(k) % NGPR]

static const struct { int op; const char *m; } ALU[] = {
    { T_OP_AND, "and" }, { T_OP_BIC, "bic" }, { T_OP_ORR, "orr" },
    { T_OP_EOR, "eor" }, { T_OP_ADD, "add" }, { T_OP_ADC, "adc" },
    { T_OP_SBC, "sbc" }, { T_OP_SUB, "sub" }, { T_OP_RSB, "rsb" }
};
#define NALU ((int)(sizeof ALU / sizeof ALU[0]))
static const char *const SH[4] = { "lsl", "lsr", "asr", "ror" };

static void data_processing(void)
{
    for (int a = 0; a < NALU; a++)
        for (int s = 0; s < 2; s++)
            for (int d = 0; d < NGPR; d++)
                for (int n = 0; n < NSOME; n++)
                    for (int m = 0; m < NGPR; m++)
                        I("a32_alu_reg", (t_alu_reg(&C, ALU[a].op, GPR[d],
                                                     SOME[n], GPR[m], s), 0),
                          "%s%s %s, %s, %s", ALU[a].m, s ? "s" : "",
                          R[GPR[d]], R[SOME[n]], R[GPR[m]]);
    for (int a = 0; a < NALU; a++)
        for (int t = 0; t < 4; t++)
            for (int sh = 1; sh <= 32; sh++) {
                if (sh == 32 && t != T_SH_LSR && t != T_SH_ASR)
                    continue;
                for (int s = 0; s < 2; s++)
                    I("a32_alu_reg_shift",
                      (t_alu_reg_shift(&C, ALU[a].op, G(sh), G(sh + 3 + a),
                                       G(sh * 5 + t), t, sh, s), 0),
                      "%s%s %s, %s, %s, %s #%d", ALU[a].m, s ? "s" : "",
                      R[G(sh)], R[G(sh + 3 + a)], R[G(sh * 5 + t)], SH[t], sh);
            }
    /* every modified immediate, with every operation and both S */
    for (int a = 0; a < NALU; a++)
        for (int s = 0; s < 2; s++)
            for (int k = 0; k < g_nimm; k++) {
                int d = G(k), n = G(k / 15);
                if (!t_imm_ok((long)g_imm[k]))
                    die("t_imm_ok refused %#x, which rotates", g_imm[k]);
                I("a32_alu_imm", t_alu_imm(&C, ALU[a].op, d, n,
                                           (long)g_imm[k], s),
                  "%s%s %s, %s, #%u", ALU[a].m, s ? "s" : "", R[d], R[n],
                  g_imm[k]);
            }
    /* ...and the forms with the flags dead: ADD/SUB of a negation, AND/BIC
     * of a complement, ORN as ORR of the complement */
    for (int k = 0; k < g_nimm; k++) {
        unsigned v = g_imm[k];
        if (!a32_imm_ok((long)(0u - v)))
            I("a32_alu_imm", t_alu_imm(&C, T_OP_ADD, 1, 2, (long)(0u - v), 0),
              "sub r1, r2, #%u", v);
        if (!a32_imm_ok((long)(0u - v)))
            I("a32_alu_imm", t_alu_imm(&C, T_OP_SUB, 3, 4, (long)(0u - v), 0),
              "add r3, r4, #%u", v);
        if (!a32_imm_ok((long)~v))
            I("a32_alu_imm", t_alu_imm(&C, T_OP_AND, 5, 6, (long)~v, 0),
              "bic r5, r6, #%u", v);
        if (!a32_imm_ok((long)~v))
            I("a32_alu_imm", t_alu_imm(&C, T_OP_BIC, 7, 8, (long)~v, 0),
              "and r7, r8, #%u", v);
        I("a32_alu_imm", t_alu_imm(&C, T_OP_ORN, 9, 10, (long)~v, 0),
          "orr r9, r10, #%u", v);
    }
    /* addw/subw: every value 0..4095, in one instruction or two */
    for (int v = 0; v < 4096; v++)
        for (int sub = 0; sub < 2; sub++) {
            int d = G(v), n = G(v / 15);
            const char *m = sub ? "sub" : "add";
            if (a32_imm_ok(v))
                I("a32_addsubw", (sub ? t_subw : t_addw)(&C, d, n, v),
                  "%s %s, %s, #%d", m, R[d], R[n], v);
            else
                I("a32_addsubw", (sub ? t_subw : t_addw)(&C, d, n, v),
                  "%s %s, %s, #%d\n%s %s, %s, #%d", m, R[d], R[n], v & 0xff0,
                  m, R[d], R[d], v & 0xf);
        }
    /* constants: mov, mvn, movw, movw + movt */
    {
        unsigned long vals[] = { 0, 1, 255, 256, 0xff00, 0xffff, 0x10000,
                                 0x12345678, 0xffffffff, 0xfffffffe,
                                 0x80000000, 0x7fffffff, 0xff00ff00,
                                 0xdeadbeef, 0x0000beef, 0x00abc000,
                                 0xfffff000, 0x00010001 };
        for (unsigned k = 0; k < sizeof vals / sizeof vals[0]; k++)
            for (int d = 0; d < 15; d += 6) {
                unsigned long v = vals[k];
                if (a32_imm_ok((long)v))
                    I("a32_mov_imm", (t_mov_imm(&C, d, (long)v, 0), 0),
                      "mov %s, #%lu", R[d], v);
                else if (a32_imm_ok((long)(~v & 0xffffffffUL)))
                    I("a32_mov_imm", (t_mov_imm(&C, d, (long)v, 1), 0),
                      "mvn %s, #%lu", R[d], ~v & 0xffffffffUL);
                else if (v <= 0xffff)
                    I("a32_mov_imm", (t_mov_imm(&C, d, (long)v, 0), 0),
                      "movw %s, #%lu", R[d], v);
                else
                    I("a32_mov_imm", (t_mov_imm(&C, d, (long)v, 0), 0),
                      "movw %s, #%lu\nmovt %s, #%lu", R[d], v & 0xffff,
                      R[d], v >> 16);
            }
        for (unsigned v = 0; v < 65536; v += 257)
            I("a32_movw_movt", (t_movw_movt(&C, G(v), v, 0), 0),
              "movw %s, #%u", R[G(v)], v);
        for (unsigned v = 0; v < 65536; v += 257)
            I("a32_movw_movt", (t_movw_movt(&C, G(v), v, 1), 0),
              "movt %s, #%u", R[G(v)], v);
        I("a32_mov_addr", t_mov_addr(&C, 3, 0x12345678),
          "movw r3, #22136\nmovt r3, #4660");
        for (int k = 0; k < g_nimm; k += 7)
            I("a32_movs_imm", t_movs_imm(&C, G(k), (long)g_imm[k]),
              "movs %s, #%u", R[G(k)], g_imm[k]);
        for (int k = 0; k < g_nimm; k += 7)
            if (!a32_imm_ok((long)~g_imm[k]))
                I("a32_ldr_const", t_ldr_const(&C, G(k), ~g_imm[k]),
                  "mvn %s, #%u", R[G(k)], g_imm[k]);
        I("a32_ldr_const", t_ldr_const(&C, 2, 0x1234), "movw r2, #4660");
        I("a32_ldr_const", t_ldr_const(&C, 2, 0x40), "mov r2, #64");
    }
    for (int d = 0; d < 15; d++)
        for (int m = 0; m < 15; m++) {
            if (d == 13 || m == 13)
                continue;
            if (d != m)
                I("a32_mov_reg", (t_mov_reg(&C, d, m), 0), "mov %s, %s",
                  R[d], R[m]);
            I("a32_movs_reg", (t_movs_reg(&C, d, m), 0), "movs %s, %s",
              R[d], R[m]);
            I("a32_mvn_reg", (t_mvn_reg(&C, d, m, 0), 0), "mvn %s, %s",
              R[d], R[m]);
            I("a32_mvn_reg", (t_mvn_reg(&C, d, m, 1), 0), "mvns %s, %s",
              R[d], R[m]);
            I("a32_cmp_reg", (t_cmp_reg(&C, d, m), 0), "cmp %s, %s",
              R[d], R[m]);
            I("a32_tst_reg", (t_tst_reg(&C, d, m), 0), "tst %s, %s",
              R[d], R[m]);
        }
    for (int t = 0; t < 4; t++)
        for (int sh = 0; sh <= 32; sh++)
            for (int s = 0; s < 2; s++) {
                int d = G(sh + t), m = G(sh * 3 + 1 + s);
                if (sh == 0 && t != T_SH_LSL)
                    continue;
                if (sh == 32 && t != T_SH_LSR && t != T_SH_ASR)
                    continue;
                if (sh == 0 && !s && d == m)
                    continue;
                if (sh == 0)
                    I("a32_shift_imm", (t_shift_imm(&C, t, d, m, 0, s), 0),
                      "mov%s %s, %s", s ? "s" : "", R[d], R[m]);
                else
                    I("a32_shift_imm", (t_shift_imm(&C, t, d, m, sh, s), 0),
                      "%s%s %s, %s, #%d", SH[t], s ? "s" : "", R[d], R[m], sh);
            }
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < 15 * 3; k++)
            for (int s = 0; s < 2; s++)
                I("a32_shift_reg", (t_shift_reg(&C, t, G(k), G(k * 7),
                                                G(k * 11 + 2), s), 0),
                  "%s%s %s, %s, %s", SH[t], s ? "s" : "", R[G(k)],
                  R[G(k * 7)], R[G(k * 11 + 2)]);
    for (int k = 0; k < g_nimm; k++) {
        I("a32_cmp_imm", (t_cmp_imm(&C, k % 15, (long)g_imm[k]), 0),
          "cmp %s, #%u", R[k % 15], g_imm[k]);
        if (!a32_imm_ok((long)(0u - g_imm[k])))
            I("a32_cmp_imm", (t_cmp_imm(&C, k % 15, (long)(0u - g_imm[k])), 0),
              "cmn %s, #%u", R[k % 15], g_imm[k]);
        if (k % 5 == 0)
            I("a32_tst_imm", t_tst_imm(&C, k % 15, (long)g_imm[k]),
              "tst %s, #%u", R[k % 15], g_imm[k]);
    }
    for (int k = 0; k < 15 * 15; k++) {
        int d = G(k), n = G(k / 15), m = G(k * 7 + 3), a = G(k * 5 + 1);
        I("a32_mul", (t_mul(&C, d, n, m), 0), "mul %s, %s, %s", R[d], R[n], R[m]);
        I("a32_mla", (t_mla(&C, d, n, m, a), 0), "mla %s, %s, %s, %s",
          R[d], R[n], R[m], R[a]);
        I("a32_mla", (t_mls(&C, d, n, m, a), 0), "mls %s, %s, %s, %s",
          R[d], R[n], R[m], R[a]);
        if (d != a)
            for (int sg = 0; sg < 2; sg++) {
                I("a32_mull", (t_mull(&C, d, a, n, m, sg), 0),
                  "%s %s, %s, %s, %s", sg ? "smull" : "umull", R[d], R[a],
                  R[n], R[m]);
                I("a32_mlal", (t_mlal(&C, d, a, n, m, sg), 0),
                  "%s %s, %s, %s, %s", sg ? "smlal" : "umlal", R[d], R[a],
                  R[n], R[m]);
            }
        I("a32_smmul", (t_smmul(&C, d, n, m), 0), "smmul %s, %s, %s",
          R[d], R[n], R[m]);
        for (int sg = 0; sg < 2; sg++)
            I("a32_div", (t_div(&C, d, n, m, sg), 0), "%s %s, %s, %s",
              sg ? "sdiv" : "udiv", R[d], R[n], R[m]);
    }
    for (int d = 0; d < 15; d++)
        for (int m = 0; m < 15; m++) {
            if (d == 13 || m == 13)
                continue;
            I("a32_ext", (t_ext(&C, d, m, 1, 0), 0), "uxtb %s, %s", R[d], R[m]);
            I("a32_ext", (t_ext(&C, d, m, 1, 1), 0), "sxtb %s, %s", R[d], R[m]);
            I("a32_ext", (t_ext(&C, d, m, 2, 0), 0), "uxth %s, %s", R[d], R[m]);
            I("a32_ext", (t_ext(&C, d, m, 2, 1), 0), "sxth %s, %s", R[d], R[m]);
            I("a32_bitop", (t_clz(&C, d, m), 0), "clz %s, %s", R[d], R[m]);
            I("a32_bitop", (t_rev(&C, d, m), 0), "rev %s, %s", R[d], R[m]);
            I("a32_bitop", (t_rev16(&C, d, m), 0), "rev16 %s, %s", R[d], R[m]);
            I("a32_bitop", (t_rbit(&C, d, m), 0), "rbit %s, %s", R[d], R[m]);
        }
    for (int lsb = 0; lsb < 32; lsb++)
        for (int w = 1; lsb + w <= 32; w++)
            for (int sg = 0; sg < 2; sg++)
                I("a32_bfx", (t_bfx(&C, G(lsb + w), G(lsb * 3 + w), lsb, w,
                                    sg), 0),
                  "%s %s, %s, #%d, #%d", sg ? "sbfx" : "ubfx",
                  R[G(lsb + w)], R[G(lsb * 3 + w)], lsb, w);
}

static const char *ldst_mn(int size, int sign, int store)
{
    if (store)
        return size == 1 ? "strb" : size == 2 ? "strh" : "str";
    if (size == 1) return sign ? "ldrsb" : "ldrb";
    if (size == 2) return sign ? "ldrsh" : "ldrh";
    return "ldr";
}

static void memory(void)
{
    static const long offs[] = { -4095, -4094, -1000, -256, -255, -254, -16,
                                 -1, 0, 1, 3, 16, 254, 255, 256, 1000, 4094,
                                 4095 };
    for (int size = 1; size <= 4; size *= 2)
        for (int sign = 0; sign < 2; sign++)
            for (int store = 0; store < 2; store++) {
                if (sign && (store || size == 4))
                    continue;
                int wide = size == 4 || (size == 1 && !sign);
                for (unsigned k = 0; k < sizeof offs / sizeof offs[0]; k++) {
                    long off = offs[k];
                    if (!wide && (off < -255 || off > 255))
                        continue;
                    for (int r = 0; r < NGPR; r++) {
                        int t = GPR[r], n = (r * 7 + (int)k) % 15;
                        I("a32_ldst_imm", t_ldst_imm(&C, t, n, off, size, sign,
                                                     store),
                          "%s %s, [%s, #%ld]", ldst_mn(size, sign, store),
                          R[t], R[n], off);
                    }
                    for (int pre = 0; pre < 2; pre++) {
                        int t = (int)k % 13, n = ((int)k + 5) % 13;
                        if (t == n) n = 14;
                        if (pre)
                            I("a32_ldst_wb", t_ldst_wb(&C, t, n, off, size,
                                                       sign, store, 1),
                              "%s %s, [%s, #%ld]!", ldst_mn(size, sign, store),
                              R[t], R[n], off);
                        else
                            I("a32_ldst_wb", t_ldst_wb(&C, t, n, off, size,
                                                       sign, store, 0),
                              "%s %s, [%s], #%ld", ldst_mn(size, sign, store),
                              R[t], R[n], off);
                    }
                }
                for (int sh = 0; sh < 32; sh++) {
                    if (!wide && sh)
                        break;
                    int t = G(sh), n = G(sh + 4), m = G(sh * 3 + 1);
                    if (!t_ldst_reg_ok(sh, size, sign, store))
                        die("t_ldst_reg_ok refused a form A32 has");
                    if (sh)
                        I("a32_ldst_reg", (t_ldst_reg(&C, t, n, m, sh, size,
                                                      sign, store), 0),
                          "%s %s, [%s, %s, lsl #%d]",
                          ldst_mn(size, sign, store), R[t], R[n], R[m], sh);
                    else
                        I("a32_ldst_reg", (t_ldst_reg(&C, t, n, m, 0, size,
                                                      sign, store), 0),
                          "%s %s, [%s, %s]", ldst_mn(size, sign, store),
                          R[t], R[n], R[m]);
                }
            }
    for (int rt = 0; rt < 14; rt += 2)
        for (long off = -255; off <= 255; off += 17)
            for (int st = 0; st < 2; st++)
                I("a32_ldst_pair", t_ldst_pair(&C, rt, rt + 1, (rt + 3) % 15,
                                               off, st),
                  "%s %s, %s, [%s, #%ld]", st ? "strd" : "ldrd", R[rt],
                  R[rt + 1], R[(rt + 3) % 15], off);
    {
        static const long sp_offs[] = { 0, 4, 255, 256, 1020, 1021, 4095 };
        for (unsigned k = 0; k < sizeof sp_offs / sizeof sp_offs[0]; k++) {
            long v = sp_offs[k];
            if (a32_imm_ok(v)) {
                I("a32_add_sp", (t_add_sp(&C, 2, v), 0), "add r2, sp, #%ld", v);
                I("a32_sp_adjust", (t_sp_adjust(&C, v, 1), 0),
                  "sub sp, sp, #%ld", v);
                I("a32_sp_adjust", (t_sp_adjust(&C, v, 0), 0),
                  "add sp, sp, #%ld", v);
            } else {
                I("a32_add_sp", (t_add_sp(&C, 2, v), 0),
                  "add r2, sp, #%ld\nadd r2, r2, #%ld", v & 0xff0, v & 15);
                I("a32_sp_adjust", (t_sp_adjust(&C, v, 1), 0),
                  "sub sp, sp, #%ld\nsub sp, sp, #%ld", v & 0xff0, v & 15);
            }
        }
        I("a32_add_sp", (t_add_sp(&C, 4, 0x12345), 0),
          "movw r4, #9029\nmovt r4, #1\nadd r4, sp, r4");
        I("a32_add_sp", (t_add_sp(&C, 4, -8), 0), "sub r4, sp, #8");
        I("a32_sp_adjust", (t_sp_adjust(&C, 0x10000, 1), 0),
          "mov r12, #65536\nsub sp, sp, r12");
        I("a32_sp_adjust", (t_sp_adjust(&C, 0x12345, 0), 0),
          "movw r12, #9029\nmovt r12, #1\nadd sp, sp, r12");
    }
    {
        unsigned masks[] = { 0x0010, 0x4010, 0x4ff0, 0x0001, 0x5fff, 0x0ff0,
                             0x4008 };
        for (unsigned k = 0; k < sizeof masks / sizeof masks[0]; k++) {
            I("a32_push", t_push(&C, masks[k]), "stmdb sp!, %s",
              rlist(masks[k]));
            unsigned pm = (masks[k] & ~0x4000u) |
                          (masks[k] & 0x4000u ? 0x8000u : 0);
            I("a32_pop", t_pop(&C, pm), "ldm sp!, %s", rlist(pm));
        }
        int at = t_push(&C, 0x4010);
        t_patch_push(&C, at, 0x4ff0);
        say(at, "a32_patch_mask", "stmdb sp!, %s", rlist(0x4ff0));
        at = t_pop(&C, 0x8010);
        t_patch_pop(&C, at, 0x8ff0);
        say(at, "a32_patch_mask", "ldm sp!, %s", rlist(0x8ff0));
        for (int rn = 0; rn < 15; rn++)
            for (int v = 0; v < 4; v++) {
                unsigned m = (0x5u << (rn % 4)) & ~(1u << rn) & ~0x2000u;
                int load = v & 1, before = v >> 1;
                if (rn == 13)
                    continue;
                I("a32_ldm_stm", t_ldm_stm(&C, rn, m, 1, before, load),
                  "%s%s %s!, %s", load ? "ldm" : "stm", before ? "db" : "ia",
                  R[rn], rlist(m));
                I("a32_ldm_stm", t_ldm_stm(&C, rn, m | 0x200, 0, before, load),
                  "%s%s %s, %s", load ? "ldm" : "stm", before ? "db" : "ia",
                  R[rn], rlist(m | 0x200));
            }
    }
    for (int t = 0; t < 13; t++) {
        int n = (t + 5) % 13, d = (t + 9) % 13;
        I("a32_ldrex", (t_ldrex(&C, t, n, 0), 0), "ldrex %s, [%s]", R[t], R[n]);
        I("a32_ldrex", (t_ldrexbh(&C, t, n, 1), 0), "ldrexb %s, [%s]", R[t], R[n]);
        I("a32_ldrex", (t_ldrexbh(&C, t, n, 2), 0), "ldrexh %s, [%s]", R[t], R[n]);
        I("a32_strex", (t_strex(&C, d, t, n, 0), 0), "strex %s, %s, [%s]",
          R[d], R[t], R[n]);
        I("a32_strex", (t_strexbh(&C, d, t, n, 1), 0), "strexb %s, %s, [%s]",
          R[d], R[t], R[n]);
        I("a32_strex", (t_strexbh(&C, d, t, n, 2), 0), "strexh %s, %s, [%s]",
          R[d], R[t], R[n]);
        if (!(t & 1) && t < 12) {
            I("a32_ldrex", (a32_ldrex(&C, t, n, 8), 0), "ldrexd %s, %s, [%s]",
              R[t], R[t + 1], R[n]);
            I("a32_strex", (a32_strex(&C, d, t, n, 8), 0),
              "strexd %s, %s, %s, [%s]", R[d], R[t], R[t + 1], R[n]);
        }
    }
    for (long off = -4095; off <= 4095; off += 455)
        I("a32_ldr_lit", t_ldr_lit(&C, (int)(off & 7), off),
          "ldr %s, [pc, #%ld]", R[off & 7], off);
}

static void control(void)
{
    /* branches, patched to every distance the field can say at its ends */
    static const long offs[] = { -33554432, -33554428, -4096, -8, -4, 0, 4, 8,
                                 4096, 33554424, 33554428 };
    for (unsigned k = 0; k < sizeof offs / sizeof offs[0]; k++) {
        long d = offs[k];
        /* the branch goes at `at`; its target is at + 8 + d -- placed in
         * the listing as `.+(8 + d)` */
        int at = t_b(&C);
        if (!a32_patch_b(&C, at, (int)(at + 8 + d)))
            die("a32_patch_b refused %ld", d);
        say(at, "a32_b", "b .%+ld", 8 + d);
        at = t_bl(&C);
        t_patch_bl(&C, at, (int)(at + 8 + d));
        say(at, "a32_patch_b", "bl .%+ld", 8 + d);
        for (int cond = 0; cond < 14; cond++) {
            at = t_bcond(&C, cond);
            t_patch_bcond(&C, at, (int)(at + 8 + d));
            say(at, "a32_b", "b%s .%+ld", COND[cond], 8 + d);
        }
    }
    for (int cond = 0; cond < 14; cond++) {
        int at = t_bcond16(&C, cond);
        if (!t_patch_bcond16(&C, at, at + 64))
            die("t_patch_bcond16 refused a near branch");
        say(at, "a32_b", "b%s .+64", COND[cond]);
        at = t_b16(&C);
        if (!t_patch_b16(&C, at, at - 64))
            die("t_patch_b16 refused a near branch");
        say(at, "a32_patch_b", "b .-64");
    }
    for (int m = 0; m < 15; m++) {
        I("a32_bx", (t_bx(&C, m), 0), "bx %s", R[m]);
        I("a32_bx", (t_blx(&C, m), 0), "blx %s", R[m]);
    }
    I("a32_nop", (t_nop(&C), 0), "nop");
    /* the IT queue: every condition, on a spread of encoders, and a
     * three-deep then/else block */
    for (int cond = 0; cond < 14; cond++) {
        const char *cc = COND[cond];
        IC(cond, "a32_it", (t_mov_reg(&C, 1, 2), 0), "mov%s r1, r2", cc);
        IC(cond, "a32_it", (t_alu_reg(&C, T_OP_ADD, 1, 2, 3, 1), 0),
           "adds%s r1, r2, r3", cc);
        IC(cond, "a32_it", t_alu_imm(&C, T_OP_SUB, 4, 5, 255, 0),
           "sub%s r4, r5, #255", cc);
        IC(cond, "a32_it", (t_mov_imm(&C, 6, 0x12345678, 0), 0),
           "movw%s r6, #22136\nmovt%s r6, #4660", cc, cc);
        IC(cond, "a32_it", (t_addw(&C, 7, 8, 0x123), 0),
           "add%s r7, r8, #288\nadd%s r7, r7, #3", cc, cc);
        IC(cond, "a32_it", t_ldst_imm(&C, 0, 1, -200, 2, 1, 0),
           "ldrsh%s r0, [r1, #-200]", cc);
        IC(cond, "a32_it", t_ldst_imm(&C, 0, 1, 2000, 4, 0, 1),
           "str%s r0, [r1, #2000]", cc);
        IC(cond, "a32_it", (t_cmp_imm(&C, 9, 1), 0), "cmp%s r9, #1", cc);
        IC(cond, "a32_it", (t_shift_imm(&C, T_SH_ASR, 2, 3, 7, 0), 0),
           "asr%s r2, r3, #7", cc);
        IC(cond, "a32_it", t_b(&C), "b%s .+8", cc);
        IC(cond, "a32_it", (t_bx(&C, 14), 0), "bx%s lr", cc);
        IC(cond, "a32_it", t_pop(&C, 0x80f0), "pop%s {r4, r5, r6, r7, pc}", cc);
        IC(cond, "a32_it", (t_vadd(&C, 0, 1, 2, 0), 0),
           "vadd%s.f32 s0, s1, s2", cc);
        I("a32_setcc", (t_setcc_low(&C, cond, 10), 0),
          "mov%s r10, #0\nmov%s r10, #1", COND[cond ^ 1], cc);
        {
            int at = C.len;
            t_it(&C, cond, "et");
            t_mov_imm(&C, 0, 1, 0);
            t_mov_imm(&C, 0, 2, 0);
            t_mov_reg(&C, 3, 4);
            t_mov_reg(&C, 5, 6);
            say(at, "a32_it", "mov%s r0, #1\nmov%s r0, #2\nmov%s r3, r4\n"
                "mov r5, r6", cc, COND[cond ^ 1], cc);
            if (a32_it_open())
                die("an IT queue was left open");
        }
    }
    {
        int at = C.len;
        if (!a32_adr(&C, 3, 64))
            die("a32_adr refused 64");
        say(at, "a32_adr", "add r3, pc, #64");
        at = C.len;
        if (!a32_adr(&C, 3, -64))
            die("a32_adr refused -64");
        say(at, "a32_adr", "sub r3, pc, #64");
        at = C.len;
        a32_adr(&C, 5, 0);
        if (!a32_patch_adr(&C, at, 5, 1020))
            die("a32_patch_adr refused 1020");
        say(at, "a32_patch_adr", "add r5, pc, #1020");
    }
}

static void system_instructions(void)
{
    I("a32_cps", (t_cps(&C, 1, 1, 0), 0), "cpsid i");
    I("a32_cps", (t_cps(&C, 0, 1, 0), 0), "cpsie i");
    I("a32_cps", (t_cps(&C, 1, 0, 1), 0), "cpsid f");
    I("a32_cps", (t_cps(&C, 0, 1, 1), 0), "cpsie if");
    I("a32_barrier", (t_barrier(&C, T_BAR_DSB), 0), "dsb sy");
    I("a32_barrier", (t_barrier(&C, T_BAR_DMB), 0), "dmb sy");
    I("a32_barrier", (t_barrier(&C, T_BAR_ISB), 0), "isb sy");
    I("a32_clrex", (t_clrex(&C), 0), "clrex");
    I("a32_hint", (t_hint(&C, T_HINT_YIELD), 0), "yield");
    I("a32_hint", (t_hint(&C, T_HINT_WFE), 0), "wfe");
    I("a32_hint", (t_hint(&C, T_HINT_WFI), 0), "wfi");
    I("a32_hint", (t_hint(&C, T_HINT_SEV), 0), "sev");
    for (int v = 0; v < 256; v += 17) {
        I("a32_bkpt", (t_bkpt(&C, v), 0), "bkpt #%d", v);
        I("a32_svc", (t_svc(&C, v), 0), "svc #%d", v);
    }
    I("a32_bkpt", (a32_bkpt(&C, 0xffff), 0), "bkpt #65535");
    I("a32_svc", (a32_svc(&C, 0x123456), 0), "svc #1193046");
    I("a32_udf", (a32_udf(&C, 0), 0), "udf #0");
    I("a32_udf", (a32_udf(&C, 0xbeef), 0), "udf #48879");
    for (int r = 0; r < 15; r++) {
        I("a32_mrs_cpsr", (a32_mrs_cpsr(&C, r), 0), "mrs %s, apsr", R[r]);
        I("a32_msr_cpsr", (a32_msr_cpsr(&C, 1, r), 0), "msr cpsr_c, %s", R[r]);
        I("a32_msr_cpsr", (a32_msr_cpsr(&C, 9, r), 0), "msr cpsr_fc, %s", R[r]);
        I("a32_mrc_mcr", (a32_mrc_mcr(&C, 1, 15, 0, r, 1, 0, 0), 0),
          "mrc p15, #0, %s, c1, c0, #0", R[r]);
        I("a32_mrc_mcr", (a32_mrc_mcr(&C, 0, 15, 0, r, 13, 0, 4), 0),
          "mcr p15, #0, %s, c13, c0, #4", R[r]);
        I("a32_mrc_mcr", (a32_mrc_mcr(&C, 1, 14, 7, r, 15, 9, 7), 0),
          "mrc p14, #7, %s, c15, c9, #7", R[r]);
    }
}

/* The VFP forms: the Thumb encoders' 28 bits under a condition field. A
 * spread of registers each, both widths; tools/vfpcheck sweeps the field
 * packing itself in Thumb state. */
static void vfp(void)
{
    for (int k = 0; k < 32; k++) {
        int d = k, n = (k * 7 + 3) % 32, m = (k * 13 + 5) % 32;
        int dd = k % 16, dn = (k * 7 + 3) % 16, dm = (k * 13 + 5) % 16;
        I("a32_vfp_word", (t_vadd(&C, d, n, m, 0), 0),
          "vadd.f32 s%d, s%d, s%d", d, n, m);
        I("a32_vfp_word", (t_vsub(&C, dd, dn, dm, 1), 0),
          "vsub.f64 d%d, d%d, d%d", dd, dn, dm);
        I("a32_vfp_word", (t_vmul(&C, d, n, m, 0), 0),
          "vmul.f32 s%d, s%d, s%d", d, n, m);
        I("a32_vfp_word", (t_vdiv(&C, dd, dn, dm, 1), 0),
          "vdiv.f64 d%d, d%d, d%d", dd, dn, dm);
        I("a32_vfp_word", (t_vfma(&C, d, n, m, 0), 0),
          "vfma.f32 s%d, s%d, s%d", d, n, m);
        I("a32_vfp_word", (t_vmov_reg(&C, d, m, 0), 0),
          "vmov.f32 s%d, s%d", d, m);
        I("a32_vfp_word", (t_vabs(&C, dd, dm, 1), 0),
          "vabs.f64 d%d, d%d", dd, dm);
        I("a32_vfp_word", (t_vneg(&C, d, m, 0), 0), "vneg.f32 s%d, s%d", d, m);
        I("a32_vfp_word", (t_vsqrt(&C, dd, dm, 1), 0),
          "vsqrt.f64 d%d, d%d", dd, dm);
        I("a32_vfp_word", (t_vcmp(&C, d, m, 0), 0), "vcmp.f32 s%d, s%d", d, m);
        I("a32_vfp_word", (t_vcmpe(&C, dd, dm, 1), 0),
          "vcmpe.f64 d%d, d%d", dd, dm);
        I("a32_vfp_word", (t_vcvt_f_from_i(&C, d, m, 1, 0), 0),
          "vcvt.f32.s32 s%d, s%d", d, m);
        I("a32_vfp_word", (t_vcvt_i_from_f(&C, d, dm, 0, 1), 0),
          "vcvt.u32.f64 s%d, d%d", d, dm);
        I("a32_vfp_word", (t_vcvt_f_f(&C, dd, m, 1), 0),
          "vcvt.f64.f32 d%d, s%d", dd, m);
        I("a32_vfp_word", (t_vldst(&C, d, k % 15, (k - 16) * 4, 0, 0), 0),
          "vldr s%d, [%s, #%d]", d, R[k % 15], (k - 16) * 4);
        I("a32_vfp_word", (t_vldst(&C, dd, k % 15, k * 32, 1, 1), 0),
          "vstr d%d, [%s, #%d]", dd, R[k % 15], k * 32);
        I("a32_vfp_word", (t_vmov_core(&C, d, k % 13, 1), 0),
          "vmov s%d, %s", d, R[k % 13]);
        I("a32_vfp_word", (t_vmov_core(&C, d, k % 13, 0), 0),
          "vmov %s, s%d", R[k % 13], d);
        I("a32_vfp_word", (t_vmov_core_pair(&C, dd, k % 12, k % 12 + 1, 1), 0),
          "vmov d%d, %s, %s", dd, R[k % 12], R[k % 12 + 1]);
        I("a32_vfp_word", (t_vmov_core_pair(&C, dd, k % 12, k % 12 + 1, 0), 0),
          "vmov %s, %s, d%d", R[k % 12], R[k % 12 + 1], dd);
    }
    I("a32_vfp_word", (t_vmov_imm(&C, 3, 0x70, 0), 0), "vmov.f32 s3, #1.0");
    I("a32_vfp_word", (t_vpush_s(&C, 16, 4, 0), 0), "vpush {s16, s17, s18, s19}");
    I("a32_vfp_word", (t_vpush_d(&C, 8, 2, 1), 0), "vpop {d8, d9}");
    I("a32_vfp_word", (t_vmrs_apsr(&C), 0), "vmrs APSR_nzcv, fpscr");
    I("a32_vfp_word", t_vldm_vstm(&C, 2, 4, 3, 1, 0, 1),
      "vldmia r2!, {s4, s5, s6}");
}

/* ---- --refuse ---------------------------------------------------------- */

static int n_refused;

static void refused(const char *what, int ret, int want, int len0)
{
    if (ret != want)
        die("%s: answered %d where A32 has no form (want %d)", what, ret, want);
    if (C.len != len0)
        die("%s: refused, but wrote %d bytes", what, C.len - len0);
    n_refused++;
}
#define REFUSE(CALL, WANT) \
    do { int l_ = C.len; refused(#CALL, (CALL), (WANT), l_); } while (0)

static void refusals(void)
{
    REFUSE(t_alu_imm(&C, T_OP_ADD, 0, 1, 0x101, 1), 0);
    REFUSE(t_alu_imm(&C, T_OP_ADD, 0, 1, 0x101, 0), 0);
    REFUSE(t_alu_imm(&C, T_OP_AND, 0, 1, 0x00ff00ff, 0), 0);
    REFUSE(t_alu_imm(&C, T_OP_ORN, 0, 1, 0x101, 0), 0);
    REFUSE(t_alu_imm(&C, T_OP_SUB, 0, 1, -1, 1), 0);      /* subs #-1: C differs */
    REFUSE(t_imm_ok(0x101), 0);
    REFUSE(t_imm_ok(0x00ff00ff), 0);        /* Thumb's replicated pattern */
    REFUSE(t_imm_ok(0x1fe), 0);         /* an odd rotation: Thumb has it */
    REFUSE(t_imm_ok(0x3fc), 1);
    REFUSE(t_imm_ok(0xff000000), 1);
    REFUSE(t_imm_ok(0xf000000f), 1);
    REFUSE(t_imm_ok(0x7f8), 0);
    REFUSE(t_movs_imm(&C, 0, 0x101), -1);
    REFUSE(t_tst_imm(&C, 0, 0x101), 0);
    REFUSE(t_ldr_const(&C, 0, 0x12345678), 0);
    REFUSE(t_ldr_const(&C, 13, 1), 0);
    for (int size = 1; size <= 4; size *= 2)
        for (int sign = 0; sign < 2; sign++)
            for (int store = 0; store < 2; store++) {
                int wide = size == 4 || (size == 1 && (!sign || store));
                long lim = wide ? 4096 : 256;
                REFUSE(t_ldst_imm(&C, 0, 1, lim, size, sign, store), 0);
                REFUSE(t_ldst_imm(&C, 0, 1, -lim, size, sign, store), 0);
                REFUSE(t_ldst_wb(&C, 0, 1, lim, size, sign, store, 1), 0);
                REFUSE(t_ldst_wb(&C, 0, 0, 4, size, sign, store, 1), 0);
                REFUSE(t_ldst_wb(&C, 0, 15, 4, size, sign, store, 0), 0);
                REFUSE(t_ldst_reg_ok(2, size, sign, store), wide);
            }
    REFUSE(t_ldst_pair(&C, 1, 2, 3, 0, 0), 0);    /* odd first register */
    REFUSE(t_ldst_pair(&C, 0, 2, 3, 0, 0), 0);    /* not consecutive */
    REFUSE(t_ldst_pair(&C, 14, 15, 3, 0, 1), 0);  /* lr:pc */
    REFUSE(t_ldst_pair(&C, 0, 1, 3, 256, 0), 0);
    REFUSE(t_ldst_pair(&C, 0, 1, 3, -256, 1), 0);
    REFUSE(t_ldst_pair(&C, 0, 1, 15, 0, 0), 0);
    REFUSE(t_ldm_stm(&C, 0, 0, 0, 0, 1), 0);
    REFUSE(t_ldm_stm(&C, 0, 0x3, 1, 0, 1), 0);    /* rn in the list, written back */
    REFUSE(t_ldm_stm(&C, 0, 0x8002, 0, 0, 0), 0); /* a store of pc */
    REFUSE(t_ldm_stm(&C, 15, 0x6, 0, 0, 1), 0);
    REFUSE(t_ldr_lit(&C, 0, 4096), 0);
    REFUSE(t_ldr_lit16(&C, 0, 4), 0);              /* no 16-bit form */
    REFUSE(a32_adr(&C, 0, 0x101), 0);
    {
        int at = t_b(&C);
        REFUSE(a32_patch_b(&C, at, at + 8 + 33554432), 0);
        REFUSE(a32_patch_b(&C, at, at + 8 + 2), 0);
        REFUSE(t_patch_bcond16(&C, at, at + 8 + 33554432), 0);
        C.len = at;
    }
}

int main(int argc, char **argv)
{
    t_isa_a32 = 1;
    collect_imms();
    if (argc > 1 && strcmp(argv[1], "--refuse") == 0) {
        refusals();
        printf("%d operands A32 cannot say refused, nothing written\n",
               n_refused);
        return 0;
    }
    if (argc > 1)
        die("usage: a32check [--refuse]  (bytes to stdout, assembly to stderr)");
    fprintf(stderr, "\t.syntax unified\n\t.arm\n");
    data_processing();
    memory();
    control();
    system_instructions();
    vfp();
    fwrite(C.p, 1, (size_t)C.len, stdout);
    return 0;
}
