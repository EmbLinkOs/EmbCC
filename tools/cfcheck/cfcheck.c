/* Hands EmbCC's whole ColdFire vocabulary to a referee, one instruction
 * at a time.
 *
 *   cfcheck --vocab BASE   one line per form: the instruction as QEMU's
 *                          m68k disassembler prints it when the form sits
 *                          at address BASE + its offset, a '|', and the
 *                          bytes src/arch/coldfire/emit.c produced for it
 *   cfcheck --refuse N     provokes encoder check N, which must stop the
 *                          process with an internal error; `--refuse
 *                          list` prints how many there are
 *
 * There is no m68k assembler on the machines this is tested on (no
 * llvm-mc target, no binutils), so the referee runs the other way round
 * from mips-encoding.sh: tests/golden/coldfire-encoding.sh loads the bytes
 * into QEMU's mcf5208evb with the CPU stopped and has QEMU's monitor
 * disassemble them (`xp/Ni`, binutils' m68k disassembler). The expected
 * text below is written from the instruction's MEANING -- the operand as
 * the instruction uses it (a register, a displacement, an index register
 * and its scale, a branch's absolute target, an immediate's value) -- and
 * never from the fields, so a field in the wrong place, a wrong opmode or
 * size or a wrong extension word reads back as a different line; a wrong
 * LENGTH shifts every line after it.
 *
 * The text and the bytes of a line come from ONE entry, so a form printed
 * but not encoded cannot shift the comparison.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/coldfire/emit.h"

static struct code C;
static unsigned long BASE;

#define PC ((unsigned long)(BASE + (unsigned long)C.len))

static void line(int at, const char *txt)
{
    printf("%s|", txt);
    for (int k = at; k < C.len; k++)
        printf("%02x", C.p[k]);
    printf("\n");
}

/* An operand as the disassembler spells it (MIT syntax). `ext` is the
 * address of its first extension word, which a PC-relative mode is
 * measured from; `size` the operation's, which an immediate is as wide
 * as. */
static void ea_txt(char *out, size_t n, const struct cf_ea *e, int size,
                   unsigned long ext)
{
    switch (e->mode) {
    case CFM_D: case CFM_A:
        snprintf(out, n, "%%%s", cf_reg_name(e->reg));
        return;
    case CFM_IND:
        snprintf(out, n, "%%%s@", cf_reg_name(e->reg));
        return;
    case CFM_POST:
        snprintf(out, n, "%%%s@+", cf_reg_name(e->reg));
        return;
    case CFM_PRE:
        snprintf(out, n, "%%%s@-", cf_reg_name(e->reg));
        return;
    case CFM_DISP:
        snprintf(out, n, "%%%s@(%ld)", cf_reg_name(e->reg), e->disp);
        return;
    case CFM_IDX:
        /* the index's displacement is printed in hex, sign-extended to
         * the disassembler's 64-bit host word, with no 0x */
        if (e->xscale == 1)
            snprintf(out, n, "%%%s@(%lx,%%%s:l)", cf_reg_name(e->reg),
                     (unsigned long)e->disp, cf_reg_name(e->xreg));
        else
            snprintf(out, n, "%%%s@(%lx,%%%s:l:%d)", cf_reg_name(e->reg),
                     (unsigned long)e->disp, cf_reg_name(e->xreg), e->xscale);
        return;
    case CFM_ABSW: case CFM_ABSL:
        snprintf(out, n, "0x%lx", (unsigned long)e->disp & 0xffffffffUL);
        return;
    case CFM_PCDISP:
        snprintf(out, n, "%%pc@(0x%lx)",
                 (ext + (unsigned long)e->disp) & 0xffffffffUL);
        return;
    case CFM_PCIDX:
        snprintf(out, n, "%%pc@(0x%lx,%%%s:l)",
                 (ext + (unsigned long)e->disp) & 0xffffffffUL,
                 cf_reg_name(e->xreg));
        return;
    case CFM_IMM: {
        long v = e->imm;
        if (size == 1) v = (long)(signed char)(v & 0xff);
        else if (size == 2) v = (long)(short)(v & 0xffff);
        else v = (long)(int)(v & 0xffffffffL);
        snprintf(out, n, "#%ld", v);
        return;
    }
    }
}

static char sz(int size)
{
    return size == 1 ? 'b' : size == 2 ? 'w' : 'l';
}

/* ---- one entry per form ------------------------------------------------ */

static void v_move(int size, struct cf_ea s, struct cf_ea d)
{
    char a[80], b[80], t[200];
    int at = C.len;
    unsigned long ext = PC + 2;
    ea_txt(a, sizeof a, &s, size, ext);
    ea_txt(b, sizeof b, &d, size, ext + (unsigned long)cf_ea_ext_len(&s, size));
    if (d.mode == CFM_A)
        snprintf(t, sizeof t, "movea%c %s,%s", sz(size), a, b);
    else
        snprintf(t, sizeof t, "move%c %s,%s", sz(size), a, b);
    cf_move(&C, size, s, d);
    line(at, t);
}

static const char *const alu_name[] = { "add", "sub", "and", "or", "eor",
                                        "cmp" };

static void v_alu(enum cf_alu op, struct cf_ea s, int dn)
{
    char a[80], t[200];
    int at = C.len;
    ea_txt(a, sizeof a, &s, 4, PC + 2);
    snprintf(t, sizeof t, "%sl %s,%%%s", alu_name[op], a, cf_reg_name(dn));
    cf_alu(&C, op, s, dn);
    line(at, t);
}

static void v_alu_mem(enum cf_alu op, int dn, struct cf_ea d)
{
    char a[80], t[200];
    int at = C.len;
    ea_txt(a, sizeof a, &d, 4, PC + 2);
    snprintf(t, sizeof t, "%sl %%%s,%s", alu_name[op], cf_reg_name(dn), a);
    cf_alu_mem(&C, op, dn, d);
    line(at, t);
}

static void v_alua(enum cf_alu op, struct cf_ea s, int an)
{
    char a[80], t[200];
    int at = C.len;
    ea_txt(a, sizeof a, &s, 4, PC + 2);
    snprintf(t, sizeof t, "%sal %s,%%%s", alu_name[op], a, cf_reg_name(an));
    cf_alua(&C, op, s, an);
    line(at, t);
}

static void v_alu_imm(enum cf_alu op, long imm, int dn)
{
    char t[200];
    int at = C.len;
    snprintf(t, sizeof t, "%sil #%ld,%%%s", alu_name[op],
             (long)(int)(imm & 0xffffffffL), cf_reg_name(dn));
    cf_alu_imm(&C, op, imm, dn);
    line(at, t);
}

static void v_addq(int sub, int n, struct cf_ea d)
{
    char a[80], t[200];
    int at = C.len;
    ea_txt(a, sizeof a, &d, 4, PC + 2);
    snprintf(t, sizeof t, "%sql #%d,%s", sub ? "sub" : "add", n, a);
    cf_addq(&C, sub, n, d);
    line(at, t);
}

static void v_lea(struct cf_ea s, int an)
{
    char a[80], t[200];
    int at = C.len;
    ea_txt(a, sizeof a, &s, 4, PC + 2);
    snprintf(t, sizeof t, "lea %s,%%%s", a, cf_reg_name(an));
    cf_lea(&C, s, an);
    line(at, t);
}

static void v_pea(struct cf_ea s)
{
    char a[80], t[200];
    int at = C.len;
    ea_txt(a, sizeof a, &s, 4, PC + 2);
    snprintf(t, sizeof t, "pea %s", a);
    cf_pea(&C, s);
    line(at, t);
}

static void v_jump(int jsr, struct cf_ea s)
{
    char a[80], t[200];
    int at = C.len;
    ea_txt(a, sizeof a, &s, 4, PC + 2);
    snprintf(t, sizeof t, "%s %s", jsr ? "jsr" : "jmp", a);
    if (jsr) cf_jsr(&C, s);
    else     cf_jmp(&C, s);
    line(at, t);
}

static void v_clrtst(int tst, int size, struct cf_ea e)
{
    char a[80], t[200];
    int at = C.len;
    ea_txt(a, sizeof a, &e, size, PC + 2);
    snprintf(t, sizeof t, "%s%c %s", tst ? "tst" : "clr", sz(size), a);
    if (tst) cf_tst(&C, size, e);
    else     cf_clr(&C, size, e);
    line(at, t);
}

static void v_mul(int sign, int size, struct cf_ea s, int dn)
{
    char a[80], t[200];
    int at = C.len;
    /* the .l form's operand follows a second word */
    ea_txt(a, sizeof a, &s, size, PC + (size == 4 ? 4 : 2));
    snprintf(t, sizeof t, "mul%c%c %s,%%%s", sign ? 's' : 'u', sz(size), a,
             cf_reg_name(dn));
    cf_mul(&C, sign, size, s, dn);
    line(at, t);
}

/* divs.l and rems.l are one instruction to the disassembler, which names
 * the remainder register and then the quotient's: Dq / ea, the remainder
 * into Dr (Dr == Dq: no remainder). */
static void v_div(int sign, struct cf_ea s, int dr, int dq)
{
    char a[80], t[200];
    int at = C.len;
    ea_txt(a, sizeof a, &s, 4, PC + 4);
    snprintf(t, sizeof t, "div%cll %s,%%%s,%%%s", sign ? 's' : 'u', a,
             cf_reg_name(dr), cf_reg_name(dq));
    if (dr == dq) cf_div(&C, sign, s, dq);
    else          cf_rem(&C, sign, s, dr, dq);
    line(at, t);
}

static const char *const cond_name[16] = {
    "t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
    "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"
};

static void v_bcc(int cond, long disp, int wide)
{
    char t[200];
    int at = C.len;
    snprintf(t, sizeof t, "b%s%c 0x%lx", cond == 0 ? "ra" : cond_name[cond],
             wide ? 'w' : 's', (PC + 2 + (unsigned long)disp) & 0xffffffffUL);
    if (wide) cf_bcc_w(&C, cond, disp);
    else      cf_bcc_b(&C, cond, disp);
    line(at, t);
}

/* A sample of each kind of effective address, every register in the
 * fields that hold one handled by the loops that use these. */
static struct cf_ea sample(int k, int r)
{
    switch (k) {
    case 0:  return cf_dreg(r & 7);
    case 1:  return cf_areg(8 + (r & 7));
    case 2:  return cf_ind(8 + (r & 7));
    case 3:  return cf_post(8 + (r & 7));
    case 4:  return cf_pre(8 + (r & 7));
    case 5:  return cf_disp16(8 + (r & 7), r & 1 ? -32768 : 32767);
    case 6:  return cf_idx(8 + (r & 7), r & 1 ? -128 : 127, r ^ 5,
                           r % 3 == 0 ? 1 : r % 3 == 1 ? 2 : 4);
    case 7:  return cf_absl(0x40001000L + 4L * r);
    case 8:  return cf_pcdisp(r & 1 ? -2 : 32766);
    default: return cf_imm(r & 1 ? -2147483647L - 1 : 0x7fffffffL);
    }
}
#define NSAMPLE 10

static void vocab(void)
{
    /* moves: each size, each legal pair of kinds, every register in each
     * of the two register fields */
    for (int size = 1; size <= 4; size *= 2)
        for (int ks = 0; ks < NSAMPLE; ks++)
            for (int kd = 0; kd < 8; kd++)
                for (int r = 0; r < 8; r++) {
                    struct cf_ea s = sample(ks, r), d = sample(kd, 7 - r);
                    if (ks == 9)
                        s = cf_imm(size == 1 ? (r & 1 ? -128 : 127)
                                   : size == 2 ? (r & 1 ? -32768 : 32767)
                                   : (r & 1 ? -2147483647L - 1 : 0x7fffffffL));
                    if (!cf_move_ok(size, &s, &d))
                        continue;
                    v_move(size, s, d);
                }
    for (int r = 0; r < 8; r++) {
        int a = 8 + r;
        v_move(4, cf_disp16(CF_FP, -4 * (r + 1)), cf_dreg(r));
        v_move(4, cf_dreg(r), cf_disp16(CF_SP, 4 * r));
        v_move(2, cf_disp16(a, 2), cf_dreg(7 - r));
        v_move(1, cf_dreg(r), cf_disp16(a, -1));
    }
    for (long v = -128; v <= 127; v += 17)
        for (int r = 0; r < 8; r++)
            if ((v + r) % 3 == 0 || v == -128 || v == 127) {
                char t[80];
                int at = C.len;
                snprintf(t, sizeof t, "moveq #%ld,%%%s", v, cf_reg_name(r));
                cf_moveq(&C, v, r);
                line(at, t);
            }
    {
        char t[80];
        int at = C.len;
        snprintf(t, sizeof t, "moveq #127,%%d7");
        cf_moveq(&C, 127, 7);
        line(at, t);
        at = C.len;
        snprintf(t, sizeof t, "moveq #-128,%%d0");
        cf_moveq(&C, -128, 0);
        line(at, t);
    }

    /* lea, pea, jsr, jmp: the control modes */
    for (int r = 0; r < 8; r++) {
        int kinds[] = { 2, 5, 6, 7, 8 };
        for (int k = 0; k < 5; k++) {
            v_lea(sample(kinds[k], r), 8 + (7 - r));
            v_pea(sample(kinds[k], r));
            v_jump(1, sample(kinds[k], r));
            v_jump(0, sample(kinds[k], r));
        }
    }

    /* movem: every register's bit, in both directions */
    for (int r = 0; r < 16; r++) {
        char t[120];
        int at = C.len;
        snprintf(t, sizeof t, "moveml %%%s,%%sp@(%d)", cf_reg_name(r), 4 * r);
        cf_movem_store(&C, 1u << r, r ? cf_disp16(CF_SP, 4 * r)
                                      : cf_disp16(CF_SP, 0));
        line(at, t);
        at = C.len;
        snprintf(t, sizeof t, "moveml %%fp@(%d),%%%s", -4 * r - 4,
                 cf_reg_name(r));
        cf_movem_load(&C, cf_disp16(CF_FP, -4 * r - 4), 1u << r);
        line(at, t);
    }
    {
        char t[120];
        int at = C.len;
        snprintf(t, sizeof t, "moveml %%d2-%%d7/%%a2-%%a5,%%sp@");
        cf_movem_store(&C, 0x3cfcu, cf_ind(CF_SP));
        line(at, t);
        at = C.len;
        snprintf(t, sizeof t, "moveml %%a0@,%%d2-%%d7/%%a2-%%a5");
        cf_movem_load(&C, cf_ind(CF_A0), 0x3cfcu);
        line(at, t);
    }

    /* the ALU */
    for (int op = CF_ADD; op <= CF_CMP; op++)
        for (int k = 0; k < NSAMPLE; k++)
            for (int r = 0; r < 8; r++) {
                if (op == CF_EOR)
                    continue;
                if (k == 1 && (op == CF_AND || op == CF_OR))
                    continue;
                v_alu((enum cf_alu)op, sample(k, r), 7 - r);
            }
    for (int op = CF_ADD; op <= CF_EOR; op++)
        for (int k = 2; k < 8; k++)
            for (int r = 0; r < 8; r++)
                v_alu_mem((enum cf_alu)op, r, sample(k, 7 - r));
    for (int r = 0; r < 8; r++)
        v_alu_mem(CF_EOR, r, cf_dreg(7 - r));
    for (int op = CF_ADD; op <= CF_CMP; op++) {
        if (op != CF_ADD && op != CF_SUB && op != CF_CMP)
            continue;
        for (int k = 0; k < NSAMPLE; k++)
            for (int r = 0; r < 8; r++)
                v_alua((enum cf_alu)op, sample(k, r), 8 + (7 - r));
    }
    for (int op = CF_ADD; op <= CF_CMP; op++)
        for (int r = 0; r < 8; r++) {
            v_alu_imm((enum cf_alu)op, r & 1 ? -2147483647L - 1 : 0x7fffffffL,
                      r);
            v_alu_imm((enum cf_alu)op, r - 4, r);
        }
    for (int n = 1; n <= 8; n++)
        for (int k = 0; k < 8; k++) {
            v_addq(0, n, sample(k, n));
            v_addq(1, n, sample(k, 8 - n));
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            char t[80];
            int at = C.len;
            snprintf(t, sizeof t, "addxl %%%s,%%%s", cf_reg_name(y),
                     cf_reg_name(x));
            cf_addx(&C, 0, y, x);
            line(at, t);
            at = C.len;
            snprintf(t, sizeof t, "subxl %%%s,%%%s", cf_reg_name(y),
                     cf_reg_name(x));
            cf_addx(&C, 1, y, x);
            line(at, t);
        }

    /* one-register operations */
    {
        static const char *const un[] = { "negl", "negxl", "notl", "swap",
                                           "extw", "extl", "extbl" };
        for (int op = CF_NEG; op <= CF_EXTBL; op++)
            for (int r = 0; r < 8; r++) {
                char t[80];
                int at = C.len;
                snprintf(t, sizeof t, "%s %%%s", un[op], cf_reg_name(r));
                cf_unary(&C, (enum cf_un)op, r);
                line(at, t);
            }
    }
    for (int size = 1; size <= 4; size *= 2)
        for (int k = 0; k < 8; k++)
            for (int r = 0; r < 8; r++) {
                if (k == 1)
                    continue;
                v_clrtst(0, size, sample(k, r));
                v_clrtst(1, size, sample(k, 7 - r));
            }

    /* shifts */
    {
        static const char *const sh[] = { "asll", "asrl", "lsll", "lsrl" };
        for (int op = CF_ASL; op <= CF_LSR; op++)
            for (int r = 0; r < 8; r++) {
                char t[80];
                int at = C.len;
                snprintf(t, sizeof t, "%s #%d,%%%s", sh[op], r + 1,
                         cf_reg_name(7 - r));
                cf_shift_imm(&C, (enum cf_sh)op, r + 1, 7 - r);
                line(at, t);
                at = C.len;
                snprintf(t, sizeof t, "%s %%%s,%%%s", sh[op], cf_reg_name(r),
                         cf_reg_name(7 - r));
                cf_shift_reg(&C, (enum cf_sh)op, r, 7 - r);
                line(at, t);
            }
    }

    /* multiply and divide */
    for (int sign = 0; sign < 2; sign++)
        for (int r = 0; r < 8; r++) {
            int kw[] = { 0, 2, 3, 4, 5, 6, 7, 8, 9 };
            int kl[] = { 0, 2, 3, 4, 5 };
            for (int k = 0; k < 9; k++) {
                struct cf_ea s = sample(kw[k], r);
                if (kw[k] == 9)
                    s = cf_imm(r & 1 ? -32768 : 32767);
                v_mul(sign, 2, s, 7 - r);
            }
            for (int k = 0; k < 5; k++) {
                v_mul(sign, 4, sample(kl[k], r), 7 - r);
                v_div(sign, sample(kl[k], r), 7 - r, 7 - r);
                v_div(sign, sample(kl[k], r), r, 7 - r == r ? 0 : 7 - r);
            }
        }

    /* conditions and branches, at both ends of their reach */
    for (int cond = 0; cond < 16; cond++)
        for (int r = 0; r < 8; r += 7) {
            char t[80];
            int at = C.len;
            snprintf(t, sizeof t, "s%s %%%s", cond_name[cond],
                     cf_reg_name(r == 7 ? cond & 7 : r));
            cf_scc(&C, cond, r == 7 ? cond & 7 : r);
            line(at, t);
        }
    for (int cond = 0; cond < 16; cond++) {
        if (cond == 1)
            continue;
        v_bcc(cond, -32768, 1);
        v_bcc(cond, 32766, 1);
        v_bcc(cond, 2, 1);
        v_bcc(cond, -128, 0);
        v_bcc(cond, 126, 0);
        v_bcc(cond, 2, 0);
    }
    {
        char t[80];
        int at = C.len;
        snprintf(t, sizeof t, "bsrw 0x%lx", (PC + 2 + 1000) & 0xffffffffUL);
        cf_bsr_w(&C, 1000);
        line(at, t);
    }

    /* the rest */
    {
        char t[80];
        int at;
        at = C.len; cf_rts(&C);     line(at, "rts");
        at = C.len; cf_nop(&C);     line(at, "nop");
        at = C.len; cf_illegal(&C); line(at, "illegal");
        at = C.len; cf_halt(&C);    line(at, "halt");
        for (int v = 0; v < 16; v += 15) {
            at = C.len;
            snprintf(t, sizeof t, "trap #%d", v);
            cf_trap(&C, v);
            line(at, t);
        }
        for (int a = 8; a < 16; a++) {
            at = C.len;
            snprintf(t, sizeof t, "linkw %%%s,#%d", cf_reg_name(a),
                     a & 1 ? -32768 : 32767);
            cf_link(&C, a, a & 1 ? -32768 : 32767);
            line(at, t);
            at = C.len;
            snprintf(t, sizeof t, "unlk %%%s", cf_reg_name(a));
            cf_unlk(&C, a);
            line(at, t);
        }
    }
}

/* ---- the encoder's own checks ------------------------------------------ */

static void refuse(int n)
{
    struct cf_ea e;
    switch (n) {
    case 0:  cf_moveq(&C, 128, 0); break;
    case 1:  cf_moveq(&C, -129, 0); break;
    case 2:  cf_move(&C, 1, cf_dreg(0), cf_areg(CF_A0)); break;
    case 3:  cf_move(&C, 4, cf_disp16(CF_A0, 4), cf_absl(0x1000)); break;
    case 4:  cf_move(&C, 4, cf_imm(1), cf_disp16(CF_A0, 4)); break;
    case 5:  cf_move(&C, 4, cf_absl(0x1000), cf_idx(CF_A0, 0, 0, 1)); break;
    case 6:  cf_move(&C, 4, cf_dreg(0), cf_imm(0)); break;
    case 7:  cf_lea(&C, cf_dreg(0), CF_A0); break;
    case 8:  cf_lea(&C, cf_ind(CF_A0), CF_D0); break;
    case 9:  cf_movem_store(&C, 4, cf_pre(CF_SP)); break;
    case 10: cf_alu(&C, CF_EOR, cf_dreg(1), 0); break;
    case 11: cf_alu(&C, CF_AND, cf_areg(CF_A1), 0); break;
    case 12: cf_alu_mem(&C, CF_ADD, 0, cf_dreg(1)); break;
    case 13: cf_addq(&C, 0, 0, cf_dreg(0)); break;
    case 14: cf_addq(&C, 1, 9, cf_dreg(0)); break;
    case 15: cf_shift_imm(&C, CF_LSL, 0, 0); break;
    case 16: cf_shift_imm(&C, CF_ASR, 9, 0); break;
    case 17: cf_mul(&C, 1, 4, cf_absl(0x1000), 0); break;
    case 18: cf_div(&C, 1, cf_imm(3), 0); break;
    case 19: cf_rem(&C, 0, cf_dreg(1), 2, 2); break;
    case 20: cf_bcc_w(&C, CF_NE, 32768); break;
    case 21: cf_bcc_b(&C, CF_EQ, 0); break;
    case 22: cf_bcc_b(&C, CF_EQ, 128); break;
    case 23: cf_link(&C, CF_FP, -32769); break;
    case 24: e = cf_disp16(CF_A0, 32768); (void)e; break;
    case 25: e = cf_idx(CF_A0, 128, 0, 1); (void)e; break;
    case 26: e = cf_idx(CF_A0, 0, 0, 8); (void)e; break;
    case 27: cf_scc(&C, CF_EQ, CF_A0); break;
    case 28: cf_unary(&C, CF_NEG, CF_A1); break;
    case 29: cf_alu_imm(&C, CF_ADD, 0x100000000L, 0); break;
    case 30: cf_move(&C, 2, cf_imm(65536), cf_dreg(0)); break;
    case 31: cf_alua(&C, CF_AND, cf_dreg(0), CF_A0); break;
    case 32: cf_clr(&C, 4, cf_areg(CF_A0)); break;
    case 33: cf_jsr(&C, cf_dreg(0)); break;
    case 34: cf_bcc_w(&C, CF_F, 2); break;
    case 35: cf_addx(&C, 0, CF_A0, 0); break;
    case 36: cf_mul(&C, 0, 2, cf_areg(CF_A0), 0); break;
    case 37: cf_movem_load(&C, cf_disp16(CF_SP, 0), 0); break;
    default:
        fprintf(stderr, "cfcheck: no refusal %d\n", n);
        exit(2);
    }
    /* reached only when the check did not fire */
    printf("refusal %d emitted %d bytes\n", n, C.len);
}
#define NREFUSE 38

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--vocab")) {
        BASE = strtoul(argv[2], NULL, 0);
        vocab();
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--refuse")) {
        if (!strcmp(argv[2], "list")) {
            printf("%d\n", NREFUSE);
            return 0;
        }
        refuse(atoi(argv[2]));
        return 0;
    }
    fprintf(stderr, "usage: cfcheck --vocab BASE | --refuse N|list\n");
    return 2;
}
