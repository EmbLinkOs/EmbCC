/* Hands EmbCC's whole TriCore vocabulary to QEMU's TriCore translator and
 * compares what QEMU decodes each instruction to with what its operands
 * say it must do.
 *
 *   tricorecheck --image FILE  writes the walk: an ELF for QEMU's
 *                              tricore_testboard that executes every form
 *                              src/arch/tricore/emit.c can emit, each
 *                              register number in each field and the ends
 *                              of every immediate, and then a set of
 *                              run-time checks (tc_li's sequences, the
 *                              CSFR numbers), and stops the board through
 *                              its test device: exit status 0, or the
 *                              number of the run-time check that failed
 *   tricorecheck --check LOG   reads QEMU's `-d op` log of that walk (one
 *                              instruction per translation block) and
 *                              compares each instruction's TCG operations
 *                              with what the walk expects of it
 *   tricorecheck --refuse N    provokes encoder range check N, which must
 *                              stop the process with an internal error;
 *                              `--refuse list` prints how many there are
 *
 * WHY THE TRANSLATOR. There is no TriCore assembler or disassembler here:
 * LLVM has no TriCore target and QEMU's `-d in_asm` prints TriCore code as
 * raw bytes. But QEMU decodes every instruction it executes into TCG
 * operations that name the registers and constants it found in each field
 * -- `add_i32 loc3,d4,d5` then `mov_i32 d3,loc3` is ADD d3, d4, d5 and no
 * other instruction -- and that decoder was written from the architecture
 * manual by people who are not this encoder. A field in the wrong place
 * names the wrong register; a wrong op2 is a different operation or an
 * illegal-instruction trap; a sign-extended field where the manual says
 * zero-extended is a different constant. The expectations below are
 * written from the OPERANDS, never from the encoding.
 *
 * Every instruction must actually execute to be translated, so the walk is
 * a program. Each branch, jump and call is placed out of line at P with
 * its target X = P + displacement allocated free beside it: the fall
 * through at P+4 and the landing at X both jump back into the walk, a
 * call's landing returns, so either way the walk continues. Loads and
 * stores have their base set to RAM first. Calls run on a context-save
 * list the walk builds in the board's internal data RAM.
 *
 * The TCG text is QEMU 11's; a QEMU whose translator spells an operation
 * differently fails here loudly rather than passing quietly.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/tricore/emit.h"

/* ---- the image: the board's 2 MiB code RAM at 0x80000000 --------------- */

#define RAM_BASE 0x80000000UL
#define RAM_SIZE 0x200000L
#define MAIN_AT  0x100000L          /* the straight-line walk, as an offset */
#define DATA_RAM 0xa1100000UL       /* where loads and stores point */
#define CSA_RAM  0xd0000000UL       /* the context-save areas */
#define TESTDEV  0xf0000000UL       /* a word written here ends QEMU */

static unsigned char *img;
static unsigned char *used;         /* per byte */
static long hi_water;

static void put32(long off, unsigned long w)
{
    if (off < 0 || off + 4 > RAM_SIZE)
        { fprintf(stderr, "tricorecheck: placement outside RAM\n"); exit(2); }
    for (int k = 0; k < 4; k++) {
        if (used[off + k])
            { fprintf(stderr, "tricorecheck: overlap at +0x%lx\n", off); exit(2); }
        img[off + k] = (unsigned char)(w >> (8 * k));
        used[off + k] = 1;
    }
    if (off + 4 > hi_water)
        hi_water = off + 4;
}

static int is_free(long off, long n)
{
    if (off < 0x1000 || off + n > RAM_SIZE)
        return 0;
    for (long k = 0; k < n; k++)
        if (used[off + k])
            return 0;
    return 1;
}

/* ---- the expectations -------------------------------------------------- */

#define MAXOPS 8
struct test {
    unsigned long pc;
    char text[96];
    char want[MAXOPS][128];
    int nwant;
    int optional;           /* a path the walk may not take: a branch's
                             * fall-through or landing */
};
static struct test *tests;
static int ntests, captests;
static struct test *cur;

static void begin_at(unsigned long pc, const char *fmt, ...)
{
    va_list ap;
    if (ntests == captests) {
        captests = captests ? captests * 2 : 1024;
        tests = realloc(tests, (size_t)captests * sizeof *tests);
        if (!tests) exit(2);
    }
    cur = &tests[ntests++];
    memset(cur, 0, sizeof *cur);
    cur->pc = pc;
    va_start(ap, fmt);
    vsnprintf(cur->text, sizeof cur->text, fmt, ap);
    va_end(ap);
}

/* An expected TCG operation: the text, `?` standing for any name or
 * number, `|` separating alternatives. */
static void want(const char *fmt, ...)
{
    va_list ap;
    if (cur->nwant == MAXOPS)
        { fprintf(stderr, "tricorecheck: too many ops\n"); exit(2); }
    va_start(ap, fmt);
    vsnprintf(cur->want[cur->nwant++], sizeof cur->want[0], fmt, ap);
    va_end(ap);
}

/* ---- the walk ------------------------------------------------------------ */

static struct code C;       /* scratch for one instruction */
static long pos = MAIN_AT;  /* where the straight-line walk is */

static unsigned long one(void)
{
    unsigned long w;
    if (C.len != 4)
        { fprintf(stderr, "tricorecheck: an entry made %d bytes\n", C.len); exit(2); }
    w = (unsigned long)C.p[0] | ((unsigned long)C.p[1] << 8) |
        ((unsigned long)C.p[2] << 16) | ((unsigned long)C.p[3] << 24);
    C.len = 0;
    return w;
}

/* Emit what was just encoded into C at the walk's position: one or more
 * 32-bit instructions. */
static void here(void)
{
    if (C.len % 4)
        { fprintf(stderr, "tricorecheck: a 16-bit form in the walk\n"); exit(2); }
    for (int at = 0; at < C.len; at += 4) {
        unsigned long w = (unsigned long)C.p[at] |
                          ((unsigned long)C.p[at + 1] << 8) |
                          ((unsigned long)C.p[at + 2] << 16) |
                          ((unsigned long)C.p[at + 3] << 24);
        put32(pos, w);
        pos += 4;
    }
    C.len = 0;
}

/* The same, as a test -- exactly one instruction; its expectations follow
 * with want(). */
#define T(enc, ...) do { begin_at(RAM_BASE + (unsigned long)pos, __VA_ARGS__); \
                         C.len = 0; enc; put32(pos, one()); pos += 4; } while (0)
/* A setup instruction, not itself a test. */
#define S(enc) do { enc; here(); } while (0)

static const char *D(int r) { return tc_reg_name('d', r); }
/* QEMU's TCG calls a10 "sp"; the walk's texts follow it. */
static const char *A(int r) { return r == TC_SP ? "sp" : tc_reg_name('a', r); }

/* "$0x..." as TCG prints a 32-bit constant. */
static const char *K(long long v)
{
    static char buf[8][24];
    static int k;
    k = (k + 1) & 7;
    snprintf(buf[k], sizeof buf[k], "$0x%lx",
             (unsigned long)((unsigned long long)v & 0xffffffffULL));
    return buf[k];
}

static void set_a(int a, unsigned long v)
{
    S(tc_li_a(&C, a, v));       /* one or two instructions */
}

static void base_to_ram(int a)
{
    S(tc_movh_a(&C, a, (unsigned)(DATA_RAM >> 16)));
}

/* ---- out-of-line control transfers ---------------------------------------
 *
 * place() finds a P with room for the transfer and its fall-through jump,
 * and X = P + d with room for a landing. The walk jumps to P; P+4 and X
 * each continue the walk at `back`. */
static long find_slot(long d, long need_at_x)
{
    long start = d < 0 ? 0x2000 - d : 0x2000;
    for (long p = start; p + 8 <= RAM_SIZE; p += 8) {
        long x = p + d;
        if (p >= MAIN_AT - 0x100 && p < MAIN_AT + 0x40000)
            continue;
        if (x >= MAIN_AT - 0x100 && x < MAIN_AT + 0x40000)
            continue;
        if (x + need_at_x > p && x < p + 8)
            continue;
        if (is_free(p, 8) && is_free(x, need_at_x))
            return p;
    }
    fprintf(stderr, "tricorecheck: no room for a %ld-byte transfer\n", d);
    exit(2);
}

/* The walk's own jump from `from` to `to`, checked as a test too. */
static void walk_jump(long from, long to)
{
    begin_at(RAM_BASE + (unsigned long)from, "j +%ld (the walk's own)",
             to - from);
    want("mov_i32 PC,%s", K((long long)(RAM_BASE + (unsigned long)to)));
    cur->optional = 1;
    C.len = 0;
    tc_w(&C, tc_enc_j(to - from));
    put32(from, one());
}

enum land { L_BACK, L_RET, L_JI_A11 };

/* Place the transfer `w` (already encoded for displacement d) out of line,
 * its landing doing `land`; the caller adds the test's expectations after.
 * Returns P. */
static long place(unsigned long w, long d, enum land land, const char *text)
{
    long p = find_slot(d, 4), x = p + d, back;
    walk_jump(pos, p);
    pos += 4;
    back = pos;
    put32(p, w);
    walk_jump(p + 4, back);
    switch (land) {
    case L_BACK:
        walk_jump(x, back);
        break;
    case L_RET:
        begin_at(RAM_BASE + (unsigned long)x, "ret (a call's landing)");
        want("call ret,$0x0,$0,env");
        cur->optional = 1;
        tc_ret(&C);
        put32(x, one());
        break;
    case L_JI_A11:
        begin_at(RAM_BASE + (unsigned long)x, "ji a11 (a jl's landing)");
        want("and_i32 PC,a11,$0xfffffffe");
        cur->optional = 1;
        tc_ji(&C, TC_RA);
        put32(x, one());
        break;
    }
    begin_at(RAM_BASE + (unsigned long)p, "%s", text);
    return p;
}

/* ---- the forms ------------------------------------------------------------ */

static const int REGS[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                              14, 15 };

/* Each register in each field: the other fields take 3, 4, 5 (or the
 * next ones along when the swept one collides, so a test never has two
 * fields equal by accident). */
static void sweep3(int k, int *x, int *y, int *z)
{
    int f = k / 16, r = REGS[k % 16];
    int o[3], next = 3;
    for (int j = 0; j < 3; j++) {
        if (j == f) { o[j] = r; continue; }
        while (next == r) next++;
        o[j] = next++;
    }
    *x = o[0]; *y = o[1]; *z = o[2];
}

static const char *tcg_cond(int op)
{
    switch (op) {
    case TC_EQ: return "eq";   case TC_NE: return "ne";
    case TC_LT: return "lt";   case TC_LTU: return "ltu";
    case TC_GE: return "ge";   default:    return "geu";
    }
}

static void alu_rr(void)
{
    static const struct { int op; const char *nm; } ops[] = {
        { TC_ADD, "add" }, { TC_SUB, "sub" }, { TC_ADDX, "addx" },
        { TC_ADDC, "addc" }, { TC_SUBX, "subx" }, { TC_SUBC, "subc" },
        { TC_EQ, "eq" }, { TC_NE, "ne" }, { TC_LT, "lt" }, { TC_LTU, "lt.u" },
        { TC_GE, "ge" }, { TC_GEU, "ge.u" }, { TC_MIN, "min" },
        { TC_MINU, "min.u" }, { TC_MAX, "max" }, { TC_MAXU, "max.u" },
        { TC_AND, "and" }, { TC_OR, "or" }, { TC_XOR, "xor" },
        { TC_NOR, "nor" }, { TC_ANDN, "andn" }, { TC_ORN, "orn" },
        { TC_NAND, "nand" }, { TC_XNOR, "xnor" }, { TC_SH, "sh" },
        { TC_SHA, "sha" }, { TC_MUL, "mul" }
    };
    for (unsigned n = 0; n < sizeof ops / sizeof ops[0]; n++)
        for (int k = 0; k < 48; k++) {
            int dc, da, db, op = ops[n].op;
            sweep3(k, &dc, &da, &db);
            T(tc_alu(&C, op, dc, da, db), "%s %s, %s, %s", ops[n].nm, D(dc),
              D(da), D(db));
            switch (op) {
            case TC_ADD: want("add_i32 ?,%s,%s", D(da), D(db));
                         want("mov_i32 %s,?", D(dc)); break;
            case TC_SUB: want("sub_i32 ?,%s,%s", D(da), D(db));
                         want("mov_i32 %s,?", D(dc)); break;
            case TC_ADDX: want("addco_i32 ?,%s,%s", D(da), D(db));
                          want("mov_i32 %s,?", D(dc)); break;
            case TC_ADDC: want("addcio_i32 ?,%s,%s", D(da), D(db));
                          want("mov_i32 %s,?", D(dc)); break;
            case TC_SUBX: want("sub_i32 ?,%s,%s", D(da), D(db));
                          want("setcond_i32 PSW_C,%s,%s,geu", D(da), D(db));
                          want("mov_i32 %s,?", D(dc)); break;
            case TC_SUBC: want("not_i32 ?,%s", D(db));
                          want("addcio_i32 ?,%s,?", D(da));
                          want("mov_i32 %s,?", D(dc)); break;
            case TC_EQ: case TC_NE: case TC_LT: case TC_LTU:
            case TC_GE: case TC_GEU:
                want("setcond_i32 %s,%s,%s,%s", D(dc), D(da), D(db),
                     tcg_cond(op));
                break;
            case TC_MIN: case TC_MINU: case TC_MAX: case TC_MAXU:
                want("movcond_i32 %s,%s,%s,%s,%s,%s", D(dc), D(da), D(db),
                     D(da), D(db), op == TC_MIN ? "lt" : op == TC_MINU ? "ltu"
                                 : op == TC_MAX ? "gt" : "gtu");
                break;
            case TC_AND: want("and_i32 %s,%s,%s", D(dc), D(da), D(db)); break;
            case TC_OR:  want("or_i32 %s,%s,%s", D(dc), D(da), D(db)); break;
            case TC_XOR: want("xor_i32 %s,%s,%s", D(dc), D(da), D(db)); break;
            case TC_NOR: want("or_i32 %s,%s,%s", D(dc), D(da), D(db));
                         want("not_i32 %s,%s", D(dc), D(dc)); break;
            case TC_ANDN: want("andc_i32 %s,%s,%s", D(dc), D(da), D(db)); break;
            case TC_ORN: want("orc_i32 %s,%s,%s", D(dc), D(da), D(db)); break;
            case TC_NAND: want("and_i32 %s,%s,%s", D(dc), D(da), D(db));
                          want("not_i32 %s,%s", D(dc), D(dc)); break;
            case TC_XNOR: want("eqv_i32 %s,%s,%s", D(dc), D(da), D(db)); break;
            case TC_SH: want("call sh,?,?,%s,%s,%s", D(dc), D(da), D(db)); break;
            case TC_SHA: want("call sha,?,?,%s,env,%s,%s", D(dc), D(da), D(db));
                         break;
            case TC_MUL: want("ext_i32_i64 ?,%s", D(da));
                         want("ext_i32_i64 ?,%s", D(db));
                         want("mul_i64 ?,?,?");
                         want("mov_i32 %s,?", D(dc)); break;
            }
        }
}

/* AND/OR/XOR with a constant, as TCG's own front end lowers them: an AND
 * with a run of low ones becomes an extract. */
static void want_logic_imm(int op, int dc, int da, long long k)
{
    unsigned long u = (unsigned long)k & 0xffffffffUL;
    switch (op) {
    case TC_AND:
        if ((u & (u + 1)) == 0) {
            int w = 0;
            while (u >> w & 1) w++;
            want("extract_i32 %s,%s,$0x0,%s", D(dc), D(da), K(w));
        } else {
            want("and_i32 %s,%s,%s", D(dc), D(da), K(k));
        }
        return;
    case TC_OR:   want("or_i32 %s,%s,%s", D(dc), D(da), K(k)); return;
    case TC_XOR:  want("xor_i32 %s,%s,%s", D(dc), D(da), K(k)); return;
    case TC_ANDN: want("and_i32 %s,%s,%s", D(dc), D(da), K(~u)); return;
    case TC_ORN:  want("or_i32 %s,%s,%s", D(dc), D(da), K(~u)); return;
    case TC_NOR:  want("mov_i32 ?,%s", K(k));
                  want("or_i32 %s,%s,?", D(dc), D(da));
                  want("not_i32 %s,%s", D(dc), D(dc)); return;
    case TC_NAND: want("mov_i32 ?,%s", K(k));
                  want("and_i32 %s,%s,?", D(dc), D(da));
                  want("not_i32 %s,%s", D(dc), D(dc)); return;
    default:      want("xor_i32 %s,%s,%s", D(dc), D(da), K(k));   /* XNOR */
                  want("not_i32 %s,%s", D(dc), D(dc)); return;
    }
}

static void alu_imm(void)
{
    static const struct { int op; const char *nm; } ops[] = {
        { TC_ADD, "add" }, { TC_ADDX, "addx" }, { TC_ADDC, "addc" },
        { TC_EQ, "eq" }, { TC_NE, "ne" }, { TC_LT, "lt" }, { TC_LTU, "lt.u" },
        { TC_GE, "ge" }, { TC_GEU, "ge.u" }, { TC_MIN, "min" },
        { TC_MINU, "min.u" }, { TC_MAX, "max" }, { TC_MAXU, "max.u" },
        { TC_AND, "and" }, { TC_OR, "or" }, { TC_XOR, "xor" },
        { TC_NOR, "nor" }, { TC_ANDN, "andn" }, { TC_ORN, "orn" },
        { TC_NAND, "nand" }, { TC_XNOR, "xnor" }, { TC_SH, "sh" },
        { TC_SHA, "sha" }, { TC_MUL, "mul" }, { TC_RSUB, "rsub" }
    };
    static const long long SK[] = { -256, -255, -170, -2, -1, 1, 2, 85, 254, 255 };
    static const long long UK[] = { 1, 2, 85, 170, 255, 256, 257, 341, 510, 511 };
    static const long long HK[] = { -31, -30, -17, -2, -1, 1, 2, 17, 30, 31 };
    for (unsigned n = 0; n < sizeof ops / sizeof ops[0]; n++) {
        int op = ops[n].op;
        const long long *ks = op == TC_SH || op == TC_SHA ? HK
                            : tc_alu_imm_ok(op, 511) ? UK : SK;
        /* every register in the two register fields, then the constants */
        for (int k = 0; k < 32 + 10; k++) {
            int dc = 3, da = 4;
            long long imm = ks[k % 10];
            if (k < 16) { dc = REGS[k]; da = dc == 4 ? 5 : 4; imm = ks[3]; }
            else if (k < 32) { da = REGS[k - 16]; dc = da == 3 ? 6 : 3;
                               imm = ks[6]; }
            if (!tc_alu_imm_ok(op, imm))
                { fprintf(stderr, "tricorecheck: %s %lld\n", ops[n].nm, imm); exit(2); }
            T(tc_alu_imm(&C, op, dc, da, imm), "%s %s, %s, %lld", ops[n].nm,
              D(dc), D(da), imm);
            switch (op) {
            case TC_ADD: want("add_i32 ?,%s,%s", D(da), K(imm));
                         want("mov_i32 %s,?", D(dc)); break;
            case TC_ADDX: want("addco_i32 ?,%s,%s", D(da), K(imm));
                          want("mov_i32 %s,?", D(dc)); break;
            case TC_ADDC: want("addcio_i32 ?,%s,%s", D(da), K(imm));
                          want("mov_i32 %s,?", D(dc)); break;
            case TC_EQ: case TC_NE: case TC_LT: case TC_LTU:
            case TC_GE: case TC_GEU:
                want("setcond_i32 %s,%s,%s,%s", D(dc), D(da), K(imm),
                     tcg_cond(op));
                break;
            case TC_MIN: case TC_MINU: case TC_MAX: case TC_MAXU:
                want("mov_i32 ?,%s", K(imm));
                want("movcond_i32 %s,%s,?,%s,?,%s", D(dc), D(da), D(da),
                     op == TC_MIN ? "lt" : op == TC_MINU ? "ltu"
                     : op == TC_MAX ? "gt" : "gtu");
                break;
            case TC_SH:
                want(imm > 0 ? "shl_i32 %s,%s,%s" : "shr_i32 %s,%s,%s", D(dc),
                     D(da), K(imm > 0 ? imm : -imm));
                break;
            case TC_SHA:
                want(imm > 0 ? "shl_i32 %s,%s,%s" : "sar_i32 %s,%s,%s", D(dc),
                     D(da), K(imm > 0 ? imm : -imm));
                break;
            case TC_MUL: want("ext_i32_i64 ?,%s", D(da));
                         want("ext_i32_i64 ?,%s", K(imm));
                         want("mul_i64 ?,?,?");
                         want("mov_i32 %s,?", D(dc)); break;
            case TC_RSUB: want("mov_i32 ?,%s", K(imm));
                          want("sub_i32 ?,?,%s", D(da));
                          want("mov_i32 %s,?", D(dc)); break;
            default: want_logic_imm(op, dc, da, imm); break;
            }
        }
    }
}

static void moves(void)
{
    static const long long S16[] = { -32768, -32767, -21846, -2, -1, 0, 1,
                                     21845, 32766, 32767 };
    static const long long U16[] = { 0, 1, 21845, 32768, 43690, 65534, 65535 };
    for (int r = 0; r < 16; r++)
        for (int s = 0; s < 16; s++) {
            if (r == s)
                continue;           /* a move to itself translates to nothing */
            T(tc_mov(&C, r, s), "mov %s, %s", D(r), D(s));
            want("mov_i32 %s,%s", D(r), D(s));
            T(tc_mov_a(&C, r, s), "mov.a %s, %s", A(r), D(s));
            want("mov_i32 %s,%s", A(r), D(s));
            T(tc_mov_d(&C, r, s), "mov.d %s, %s", D(r), A(s));
            want("mov_i32 %s,%s", D(r), A(s));
            T(tc_mov_aa(&C, r, s), "mov.aa %s, %s", A(r), A(s));
            want("mov_i32 %s,%s", A(r), A(s));
        }
    for (int r = 0; r < 16; r++) {
        for (unsigned k = 0; k < sizeof S16 / sizeof S16[0]; k++) {
            T(tc_mov_imm(&C, r, S16[k]), "mov %s, %lld", D(r), S16[k]);
            want("mov_i32 %s,%s", D(r), K(S16[k]));
            T(tc_addi(&C, r, (r + 7) & 15, S16[k]), "addi %s, %s, %lld", D(r),
              D((r + 7) & 15), S16[k]);
            want("add_i32 ?,%s,%s", D((r + 7) & 15), K(S16[k]));
            want("mov_i32 %s,?", D(r));
            T(tc_lea(&C, r, (r + 5) & 15, S16[k]), "lea %s, [%s]%lld", A(r),
              A((r + 5) & 15), S16[k]);
            want(S16[k] ? "add_i32 %s,%s,%s" : "mov_i32 %s,%s%.0s", A(r),
                 A((r + 5) & 15), K(S16[k]));
        }
        for (unsigned k = 0; k < sizeof U16 / sizeof U16[0]; k++) {
            unsigned long h = (unsigned long)U16[k] << 16;
            T(tc_mov_u(&C, r, U16[k]), "mov.u %s, %lld", D(r), U16[k]);
            want("mov_i32 %s,%s", D(r), K(U16[k]));
            T(tc_movh(&C, r, (unsigned)U16[k]), "movh %s, %lld", D(r), U16[k]);
            want("mov_i32 %s,%s", D(r), K((long long)h));
            T(tc_movh_a(&C, r, (unsigned)U16[k]), "movh.a %s, %lld", A(r),
              U16[k]);
            want("mov_i32 %s,%s", A(r), K((long long)h));
            T(tc_addih(&C, r, (r + 3) & 15, (unsigned)U16[k]),
              "addih %s, %s, %lld", D(r), D((r + 3) & 15), U16[k]);
            want("add_i32 ?,%s,%s", D((r + 3) & 15), K((long long)h));
            want("mov_i32 %s,?", D(r));
            T(tc_addih_a(&C, r, (r + 9) & 15, (unsigned)U16[k]),
              "addih.a %s, %s, %lld", A(r), A((r + 9) & 15), U16[k]);
            want(h ? "add_i32 %s,%s,%s" : "mov_i32 %s,%s%.0s", A(r),
                 A((r + 9) & 15), K((long long)h));
        }
    }
    for (int k = 0; k < 48; k++) {
        int c, a, b;
        sweep3(k, &c, &a, &b);
        T(tc_add_a(&C, c, a, b), "add.a %s, %s, %s", A(c), A(a), A(b));
        want("add_i32 %s,%s,%s", A(c), A(a), A(b));
        T(tc_sub_a(&C, c, a, b), "sub.a %s, %s, %s", A(c), A(a), A(b));
        want("sub_i32 %s,%s,%s", A(c), A(a), A(b));
        for (int n = 0; n < 4; n++) {
            if (k % 4 != n && k > 2)
                continue;
            T(tc_addsc_a(&C, c, b, a, n), "addsc.a %s, %s, %s, %d", A(c),
              A(b), D(a), n);
            if (n) {
                want("shl_i32 ?,%s,%s", D(a), K(n));
                want("add_i32 %s,%s,?", A(c), A(b));
            } else {
                want("mov_i32 ?,%s", D(a));
                want("add_i32 %s,%s,?", A(c), A(b));
            }
        }
    }
}

static void muldiv(void)
{
    for (int k = 0; k < 48; k++) {
        int e, a, b;
        sweep3(k, &e, &a, &b);
        e &= ~1;
        if (k < 16 && (REGS[k] & 1))
            continue;
        for (int s = 0; s < 2; s++) {
            T(tc_mul64(&C, e, a, b, s), "mul%s %s, %s, %s", s ? "" : ".u",
              tc_reg_name('e', e), D(a), D(b));
            want("%s ?,%s", s ? "ext_i32_i64" : "extu_i32_i64", D(a));
            want("%s ?,%s", s ? "ext_i32_i64" : "extu_i32_i64", D(b));
            want("mul_i64 ?,?,?");
            want("extrl_i64_i32 %s,?", D(e));
            want("extrh_i64_i32 %s,?", D(e + 1));
            T(tc_div(&C, e, a, b, s), "div%s %s, %s, %s", s ? "" : ".u",
              tc_reg_name('e', e), D(a), D(b));
            want("call %s,?,?,?,env,%s,%s", s ? "divide" : "divide_u", D(a),
                 D(b));
            want("extrl_i64_i32 %s,?", D(e));
            want("extrh_i64_i32 %s,?", D(e + 1));
        }
    }
    for (int k = 0; k < 64; k++) {
        int f = k / 16, r = k % 16, o[4] = { 3, 4, 5, 6 };
        o[f] = r;
        for (int j = 0; j < 4; j++)
            if (j != f && o[j] == r)
                o[j] = (r + 8) & 15;
        /* madd d[o0] = d[o3] + d[o1] * d[o2] */
        T(tc_madd(&C, o[0], o[3], o[1], o[2]), "madd %s, %s, %s, %s", D(o[0]),
          D(o[3]), D(o[1]), D(o[2]));
        want("ext_i32_i64 ?,%s", D(o[1]));
        want("ext_i32_i64 ?,%s", D(o[3]));
        want("ext_i32_i64 ?,%s", D(o[2]));
        want("mul_i64 ?,?,?");
        want("add_i64 ?,?,?");
        want("extrl_i64_i32 %s,?", D(o[0]));
        T(tc_sel(&C, o[0], o[3], o[1], o[2]), "sel %s, %s, %s, %s", D(o[0]),
          D(o[3]), D(o[1]), D(o[2]));
        want("movcond_i32 %s,%s,$0x0,%s,%s,ne", D(o[0]), D(o[3]), D(o[1]),
             D(o[2]));
        T(tc_seln(&C, o[0], o[3], o[1], o[2]), "seln %s, %s, %s, %s", D(o[0]),
          D(o[3]), D(o[1]), D(o[2]));
        want("movcond_i32 %s,%s,$0x0,%s,%s,eq", D(o[0]), D(o[3]), D(o[1]),
             D(o[2]));
        /* dextr d[o0] = ({d[o1], d[o2]} << d[o3])[63:32] */
        T(tc_dextr_r(&C, o[0], o[1], o[2], o[3]), "dextr %s, %s, %s, %s",
          D(o[0]), D(o[1]), D(o[2]), D(o[3]));
        want("extract_i32 ?,%s,$0x0,$0x5", D(o[3]));
        want("shl_i32 ?,%s,?", D(o[1]));
        want("shr_i32 ?,%s,?", D(o[2]));
        want("or_i32 %s,?,?", D(o[0]));
    }
    for (int r = 0; r < 16; r++) {
        T(tc_clz(&C, r, (r + 1) & 15), "clz %s, %s", D(r), D((r + 1) & 15));
        want("clz_i32 %s,%s,$0x20", D(r), D((r + 1) & 15));
    }
}

static void bitfields(void)
{
    static const int PW[][2] = { { 0, 1 }, { 0, 31 }, { 1, 31 }, { 31, 1 },
                                 { 8, 5 }, { 16, 16 }, { 3, 7 }, { 24, 8 } };
    for (unsigned n = 0; n < sizeof PW / sizeof PW[0]; n++) {
        int p = PW[n][0], w = PW[n][1];
        for (int k = 0; k < 48; k++) {
            int c, a, b;
            sweep3(k, &c, &a, &b);
            if (n && k % 6)
                continue;
            for (int s = 0; s < 2; s++) {
                T(tc_extr(&C, c, a, p, w, s), "extr%s %s, %s, %d, %d",
                  s ? "" : ".u", D(c), D(a), p, w);
                /* a field that ends at bit 31 is a plain shift to TCG */
                if (p + w == 32)
                    want("%s %s,%s,%s", s ? "sar_i32" : "shr_i32", D(c), D(a),
                         K(p));
                else
                    want("%s %s,%s,%s,%s", s ? "sextract_i32" : "extract_i32",
                         D(c), D(a), K(p), K(w));
            }
            T(tc_insert(&C, c, a, b, p, w), "insert %s, %s, %s, %d, %d", D(c),
              D(a), D(b), p, w);
            want("deposit_i32 %s,%s,%s,%s,%s", D(c), D(a), D(b), K(p), K(w));
            T(tc_insert_imm(&C, c, a, (unsigned)(k & 15), p, w),
              "insert %s, %s, %d, %d, %d", D(c), D(a), k & 15, p, w);
            want("deposit_i32 %s,%s,%s,%s,%s", D(c), D(a), K(k & 15), K(p),
                 K(w));
            T(tc_dextr(&C, c, a, b, p), "dextr %s, %s, %s, %d", D(c), D(a),
              D(b), p);
            if (p)
                want("extract2_i32 %s,%s,%s,%s", D(c), D(b), D(a), K(32 - p));
            else
                want("mov_i32 %s,%s", D(c), D(a));
        }
    }
}

static void memory(void)
{
    static const long long OFF[] = { -32768, -32767, -21846, -64, -1, 0, 1,
                                     63, 64, 1023, 21845, 32767 };
    static const struct { int size, sign; const char *ld, *st, *mo; } W[] = {
        { 1, 1, "ld.b", "st.b", "noat+al+sb" },
        { 1, 0, "ld.bu", NULL, "noat+al+ub" },
        { 2, 1, "ld.h", "st.h", "noat+un+lesw" },
        { 2, 0, "ld.hu", NULL, "noat+un+leuw" },
        { 4, 0, "ld.w", "st.w", "noat+un+leul" }
    };
    for (unsigned w = 0; w < 5; w++)
        for (int k = 0; k < 32 + 12; k++) {
            int t = 3, b = 12;
            long long off = 8;
            if (k < 16) t = REGS[k];
            else if (k < 32) b = REGS[k - 16];
            else off = OFF[k - 32];
            base_to_ram(b);
            T(tc_load(&C, t, b, off, W[w].size, W[w].sign), "%s %s, [%s]%lld",
              W[w].ld, D(t), A(b), off);
            want(off ? "add_i32 ?,%s,%s" : "mov_i32 ?,%s%.0s", A(b), K(off));
            want("qemu_ld_i32 %s,?,%s,0", D(t), W[w].mo);
            if (!W[w].st)
                continue;
            base_to_ram(b);
            T(tc_store(&C, t, b, off, W[w].size), "%s %s, [%s]%lld", W[w].st,
              D(t), A(b), off);
            want(off ? "add_i32 ?,%s,%s" : "mov_i32 ?,%s%.0s", A(b), K(off));
            want("qemu_st_i32 %s,?,%s,0", D(t),
                 W[w].size == 1 ? "noat+al+ub" : W[w].size == 2
                                ? "noat+un+leuw" : "noat+un+leul");
        }
    for (int k = 0; k < 32 + 12; k++) {
        int t = 3, b = 12;
        long long off = 8;
        if (k < 16) t = REGS[k];
        else if (k < 32) b = REGS[k - 16];
        else off = OFF[k - 32];
        if (t == b)
            continue;               /* ld.a a, [a]: the base is overwritten */
        base_to_ram(b);
        T(tc_ld_a(&C, t, b, off), "ld.a %s, [%s]%lld", A(t), A(b), off);
        want(off ? "add_i32 ?,%s,%s" : "mov_i32 ?,%s%.0s", A(b), K(off));
        want("qemu_ld_i32 %s,?,noat+un+leul,0", A(t));
        base_to_ram(b);
        T(tc_st_a(&C, t, b, off), "st.a %s, [%s]%lld", A(t), A(b), off);
        want(off ? "add_i32 ?,%s,%s" : "mov_i32 ?,%s%.0s", A(b), K(off));
        want("qemu_st_i32 %s,?,noat+un+leul,0", A(t));
    }
}

/* The atomic word accesses, every register in each field and both ends
 * of the BO form's signed 10-bit offset. */
static void atomics(void)
{
    static const long long OFF[] = { -512, -511, -342, -1, 0, 1, 341, 510,
                                     511 };
    for (int k = 0; k < 32 + 9; k++) {
        int r = 4, b = 12;
        long long off = 8;
        if (k < 16) r = k;
        else if (k < 32) b = k - 16;
        else off = OFF[k - 32];
        base_to_ram(b);
        T(tc_swap_w(&C, r, b, off), "swap.w [%s]%lld, %s", A(b), off, D(r));
        want(off ? "add_i32 ?,%s,%s" : "mov_i32 ?,%s%.0s", A(b), K(off));
        want("qemu_ld_i32 ?,?,noat+un+leul,0");
        want("qemu_st_i32 %s,?,noat+un+leul,0", D(r));
        want("mov_i32 %s,?", D(r));
        if (r & 1)
            continue;
        base_to_ram(b);
        T(tc_cmpswap_w(&C, r, b, off), "cmpswap.w [%s]%lld, %s", A(b), off,
          tc_reg_name('e', r));
        want(off ? "add_i32 ?,%s,%s" : "mov_i32 ?,%s%.0s", A(b), K(off));
        want("qemu_ld_i32 ?,?,noat+un+leul,0");
        want("movcond_i32 ?,%s,?,%s,?,eq", D(r + 1), D(r));
        want("qemu_st_i32 ?,?,noat+un+leul,0");
        want("mov_i32 %s,?", D(r));
    }
}

/* A block of n bytes at a multiple of `align`, claimed. */
static long claim_aligned(long n, long align);

/* The trapping forms: the trap table at BTV is pointed at landings that
 * continue the walk, then each instruction is executed once. */
static void traps(void)
{
    long tab = claim_aligned(256, 256), back;
    set_a(14, RAM_BASE + (unsigned long)tab);
    S(tc_mov_d(&C, 4, 14));
    S(tc_mtcr(&C, TC_CSFR_BTV, 4));
    S(tc_isync(&C));
    T(tc_illegal(&C), "(undefined: op1 0x01, op2 0xff)");
    want("call raise_exception_sync,?,?,env,$0x2,$0x1");   /* class 2, IOPC */
    back = pos;
    walk_jump(tab + 2 * 32, back);                         /* class 2 */
    /* a system call returns to the instruction after it, in A11 */
    begin_at(RAM_BASE + (unsigned long)(tab + 6 * 32), "ji a11 (class 6)");
    want("and_i32 PC,a11,$0xfffffffe");
    tc_ji(&C, TC_RA);
    put32(tab + 6 * 32, one());
    for (unsigned k = 0; k < 4; k++) {
        static const unsigned SYS[] = { 0, 1, 170, 255 };
        T(tc_syscall(&C, SYS[k]), "syscall %u", SYS[k]);
        want("call raise_exception_sync,?,?,env,$0x6,%s", K(SYS[k]));
    }
}

static const char *jname(int cond)
{
    static const char *const nm[] = { "jeq", "jne", "jlt", "jlt.u", "jge",
                                      "jge.u", "jeq.a", "jne.a", "jz.a",
                                      "jnz.a" };
    return nm[cond];
}

static const char *jcond(int cond)
{
    static const char *const nm[] = { "eq", "ne", "lt", "ltu", "ge", "geu",
                                      "eq", "ne", "eq", "ne" };
    return nm[cond];
}

static void branches(void)
{
    static const long DISP[] = { -32768, -21846, -4096, -6, 8, 4094, 21844,
                                 32766 };
    static const long FAR[] = { -1048576, -699050, -131072, -6, 8, 131070,
                                699050, 1048574 };
    for (int cond = TC_JEQ; cond <= TC_JNZ_A; cond++) {
        int areg = cond >= TC_JEQ_A, one = cond >= TC_JZ_A;
        for (int k = 0; k < 32 + 8; k++) {
            int s1 = 4, s2 = 5;
            long d = 8;
            long p;
            char txt[96];
            if (k < 16) { s1 = k; s2 = k == 5 ? 6 : 5; }
            else if (k < 32) { if (one) continue; s2 = k - 16; s1 = s2 == 4 ? 7 : 4; }
            else d = DISP[k - 32];
            if (one)
                snprintf(txt, sizeof txt, "%s %s, %+ld", jname(cond), A(s1), d);
            else
                snprintf(txt, sizeof txt, "%s %s, %s, %+ld", jname(cond),
                         areg ? A(s1) : D(s1), areg ? A(s2) : D(s2), d);
            p = place(tc_enc_jcc(cond, s1, s2, d), d, L_BACK, txt);
            if (one)
                want("brcond_i32 %s,$0x0,%s,?", A(s1), jcond(cond));
            else
                want("brcond_i32 %s,%s,%s,?", areg ? A(s1) : D(s1),
                     areg ? A(s2) : D(s2), jcond(cond));
            want("mov_i32 PC,%s", K((long long)(RAM_BASE + (unsigned long)p + 4)));
            want("mov_i32 PC,%s", K((long long)(RAM_BASE + (unsigned long)(p + d))));
        }
        if (areg)
            continue;
        for (int k = 0; k < 16 + 16 + 8; k++) {
            int s1 = 4;
            long d = 8;
            long long kk = 3;
            int ks = cond != TC_JLTU && cond != TC_JGEU;
            long p;
            if (k < 16) s1 = k;
            else if (k < 32) kk = ks ? (long long)(k - 16) - 8 : k - 16;
            else d = DISP[k - 32];
            {
                char txt[96];
                snprintf(txt, sizeof txt, "%s %s, %lld, %+ld", jname(cond),
                         D(s1), kk, d);
                p = place(tc_enc_jcci(cond, s1, kk, d), d, L_BACK, txt);
            }
            want("brcond_i32 %s,%s,%s,?", D(s1), K(kk), jcond(cond));
            want("mov_i32 PC,%s", K((long long)(RAM_BASE + (unsigned long)p + 4)));
            want("mov_i32 PC,%s", K((long long)(RAM_BASE + (unsigned long)(p + d))));
        }
    }
    for (unsigned k = 0; k < sizeof FAR / sizeof FAR[0]; k++) {
        long d = FAR[k], p;
        char txt[64];
        snprintf(txt, sizeof txt, "j %+ld", d);
        p = place(tc_enc_j(d), d, L_BACK, txt);
        want("mov_i32 PC,%s", K((long long)(RAM_BASE + (unsigned long)(p + d))));
        snprintf(txt, sizeof txt, "call %+ld", d);
        p = place(tc_enc_call(d), d, L_RET, txt);
        want("call call,$0x0,$0,env,%s",
             K((long long)(RAM_BASE + (unsigned long)p + 4)));
        want("mov_i32 PC,%s", K((long long)(RAM_BASE + (unsigned long)(p + d))));
        snprintf(txt, sizeof txt, "jl %+ld", d);
        p = place(tc_enc_jl(d), d, L_JI_A11, txt);
        want("mov_i32 a11,%s", K((long long)(RAM_BASE + (unsigned long)p + 4)));
        want("mov_i32 PC,%s", K((long long)(RAM_BASE + (unsigned long)(p + d))));
    }
    /* the indirect forms: the register is pointed at a landing first */
    for (int a = 0; a < 16; a++) {
        for (int form = 0; form < 3; form++) {
            long p, x, d = 0x40;
            char txt[64];
            if (form > 0 && a == TC_RA)
                continue;           /* CALLI/JLI a11 read a11 after linking */
            p = find_slot(d, 4);
            x = p + d;
            set_a(a, RAM_BASE + (unsigned long)x);
            snprintf(txt, sizeof txt, "%s %s", form == 0 ? "ji" : form == 1
                                       ? "calli" : "jli", A(a));
            C.len = 0;
            if (form == 0) tc_ji(&C, a);
            else if (form == 1) tc_calli(&C, a);
            else tc_jli(&C, a);
            p = place(one(), d, form == 0 ? L_BACK : form == 1 ? L_RET
                                                               : L_JI_A11, txt);
            if (form == 1)
                want("call call,$0x0,$0,env,%s",
                     K((long long)(RAM_BASE + (unsigned long)p + 4)));
            want("and_i32 PC,%s,$0xfffffffe", A(a));
            if (form == 2)
                want("mov_i32 a11,%s",
                     K((long long)(RAM_BASE + (unsigned long)p + 4)));
        }
    }
}

static void system_forms(void)
{
    static const unsigned CSFR[] = { TC_CSFR_BIV, TC_CSFR_BTV, TC_CSFR_ISP };
    for (int r = 0; r < 16; r++) {
        T(tc_mtcr(&C, CSFR[r % 3], r), "mtcr 0x%x, %s", CSFR[r % 3], D(r));
        want("st_i32 %s,env,?", D(r));
        T(tc_mfcr(&C, r, CSFR[r % 3]), "mfcr %s, 0x%x", D(r), CSFR[r % 3]);
        want("ld_i32 %s,env,?", D(r));
    }
    T(tc_mfcr(&C, 4, TC_CSFR_PSW), "mfcr d4, psw");
    want("call psw_read,?,?,%s,env", D(4));
    T(tc_mtcr(&C, TC_CSFR_PSW, 4), "mtcr psw, d4");
    want("call psw_write,?,?,env,%s", D(4));
    /* the SYS forms that translate to no operation at all, and so are
     * checked only for not being an illegal instruction */
    T(tc_nop(&C), "nop");
    T(tc_isync(&C), "isync");
    T(tc_dsync(&C), "dsync");
}

/* ---- run-time checks, the board as the second referee ------------------- */

static int nfail_codes;

/* A free block of n bytes anywhere in RAM, claimed. */
static long claim(long n)
{
    return claim_aligned(n, 8);
}

static long claim_aligned(long n, long align)
{
    for (long p = 0x2000; p + n <= RAM_SIZE; p += align) {
        if (p + n > MAIN_AT - 0x100 && p < MAIN_AT + 0x40000)
            continue;
        if (is_free(p, n))
            return p;
    }
    fprintf(stderr, "tricorecheck: no room for %ld bytes\n", n);
    exit(2);
}

/* The walk stops the board with a code of its own when D[a] != D[b]: a
 * jeq over a jump to a stub that writes the code to the test device. */
static void check_eq(int da, int db)
{
    long p = claim(16);
    int code = ++nfail_codes;
    S(tc_w(&C, tc_enc_jcc(TC_JEQ, da, db, 8)));
    S(tc_w(&C, tc_enc_j(p - pos)));
    tc_mov_imm(&C, 15, code);
    put32(p, one());
    tc_movh_a(&C, 15, (unsigned)(TESTDEV >> 16));
    put32(p + 4, one());
    tc_store(&C, 15, 15, 0, 4);
    put32(p + 8, one());
    tc_w(&C, tc_enc_j(0));          /* and stay */
    put32(p + 12, one());
}

/* A table of the values tc_li must build, read with ld.w, compared with
 * what tc_li built. */
static void li_checks(void)
{
    static const long long V[] = {
        0, 1, -1, 32767, 32768, -32768, -32769, 65535, 65536, 0x7fff8000LL,
        0x7fffffffLL, 0x80000000LL, 0x8000ffffLL, 0x12345678LL, 0xffff7fffLL,
        0xffff8000LL, 0x00018000LL, 0xdeadbeefLL, 0x0000ffffLL, 0x7fff7fffLL,
        0xfffe0000LL, 0x55555555LL, 0xaaaaaaaaLL, 0x00007fffLL
    };
    int nv = (int)(sizeof V / sizeof V[0]);
    long tab = claim(4L * nv);
    for (int k = 0; k < nv; k++)
        put32(tab + 4L * k, (unsigned long)V[k] & 0xffffffffUL);
    set_a(13, RAM_BASE + (unsigned long)tab);
    for (int k = 0; k < nv; k++) {
        C.len = 0;
        tc_li(&C, 6, V[k]);
        if (C.len != tc_li_len(V[k]))
            { fprintf(stderr, "tricorecheck: tc_li_len(%lld) is %d, the "
                      "sequence %d\n", V[k], tc_li_len(V[k]), C.len); exit(1); }
        here();
        S(tc_load(&C, 7, 13, 4L * k, 4, 0));
        check_eq(6, 7);
        /* and the address form */
        S(tc_li_a(&C, 14, (unsigned long)V[k]));
        S(tc_mov_d(&C, 8, 14));
        check_eq(8, 7);
    }
}

/* MTCR then MFCR of the same register gives the value back, and each CSFR
 * number reaches a different register. */
static void csfr_checks(void)
{
    static const unsigned CSFR[] = { TC_CSFR_BIV, TC_CSFR_BTV, TC_CSFR_ISP };
    for (int k = 0; k < 3; k++) {
        S(tc_li(&C, 4, 0x1000 * (k + 1) + 0x100));
        S(tc_mtcr(&C, CSFR[k], 4));
    }
    S(tc_isync(&C));
    for (int k = 0; k < 3; k++) {
        S(tc_li(&C, 4, 0x1000 * (k + 1) + 0x100));
        S(tc_mfcr(&C, 5, CSFR[k]));
        check_eq(4, 5);
    }
}

/* ---- the prologue: a context-save list, and the end ---------------------- */

static void prologue(void)
{
    long save = pos;
    pos = 0;
    /* 32 CSAs at CSA_RAM, each linking to the next: a link word is the
     * segment in bits 19:16 and address bits 21:6 in 15:0 */
    S(tc_movh_a(&C, 12, (unsigned)(CSA_RAM >> 16)));
    for (int k = 0; k < 32; k++) {
        unsigned long link = k < 31 ? ((CSA_RAM >> 28) << 16) | (unsigned long)(k + 1)
                                    : 0;
        S(tc_li(&C, 4, (long long)link));
        S(tc_store(&C, 4, 12, 64L * k, 4));
    }
    S(tc_li(&C, 4, (long long)((CSA_RAM >> 28) << 16)));
    S(tc_mtcr(&C, TC_CSFR_FCX, 4));
    S(tc_li(&C, 4, (long long)(((CSA_RAM >> 28) << 16) | 30)));
    S(tc_mtcr(&C, TC_CSFR_LCX, 4));
    /* call depth counting off (PSW.CDC = 0x7f) */
    S(tc_mfcr(&C, 4, TC_CSFR_PSW));
    S(tc_alu_imm(&C, TC_OR, 4, 4, 0x7f));
    S(tc_mtcr(&C, TC_CSFR_PSW, 4));
    S(tc_isync(&C));
    C.len = 0;
    tc_w(&C, tc_enc_j(save - pos));
    here();
    pos = save;
}

static void epilogue(void)
{
    S(tc_mov_imm(&C, 15, 0));
    S(tc_movh_a(&C, 15, (unsigned)(TESTDEV >> 16)));
    S(tc_store(&C, 15, 15, 0, 4));
    C.len = 0;
    tc_w(&C, tc_enc_j(0));
    here();
}

static void build(void)
{
    img = calloc(RAM_SIZE, 1);
    used = calloc(RAM_SIZE, 1);
    if (!img || !used) exit(2);
    prologue();
    alu_rr();
    alu_imm();
    moves();
    muldiv();
    bitfields();
    memory();
    branches();
    system_forms();
    atomics();
    li_checks();
    csfr_checks();
    traps();
    epilogue();
}

static void put_le(FILE *f, unsigned long v, int n)
{
    for (int k = 0; k < n; k++)
        fputc((int)((v >> (8 * k)) & 0xff), f);
}

static int write_image(const char *path)
{
    FILE *f = fopen(path, "wb");
    long len = hi_water;
    if (!f) { perror(path); return 1; }
    /* ELF32 header: little-endian, EM_TRICORE (44), ET_EXEC */
    fputs("\177ELF", f);
    fputc(1, f); fputc(1, f); fputc(1, f);
    for (int k = 0; k < 9; k++) fputc(0, f);
    put_le(f, 2, 2); put_le(f, 44, 2); put_le(f, 1, 4);
    put_le(f, RAM_BASE, 4);            /* e_entry */
    put_le(f, 52, 4); put_le(f, 0, 4); /* phoff, shoff */
    put_le(f, 0, 4);                   /* flags */
    put_le(f, 52, 2); put_le(f, 32, 2); put_le(f, 1, 2);
    put_le(f, 40, 2); put_le(f, 0, 2); put_le(f, 0, 2);
    /* one PT_LOAD: the RAM image up to its last word */
    put_le(f, 1, 4); put_le(f, 84, 4);
    put_le(f, RAM_BASE, 4); put_le(f, RAM_BASE, 4);
    put_le(f, (unsigned long)len, 4); put_le(f, (unsigned long)len, 4);
    put_le(f, 7, 4); put_le(f, 4, 4);
    fwrite(img, 1, (size_t)len, f);
    return fclose(f) != 0;
}

/* ---- reading the log ------------------------------------------------------ */

struct block { unsigned long pc; char **ops; int nops; };
static struct block *blocks;
static int nblocks;

static void add_op(struct block *b, const char *s)
{
    b->ops = realloc(b->ops, (size_t)(b->nops + 1) * sizeof *b->ops);
    b->ops[b->nops] = malloc(strlen(s) + 1);
    if (!b->ops || !b->ops[b->nops]) exit(2);
    strcpy(b->ops[b->nops++], s);
}

static int read_log(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    struct block *b = NULL;
    if (!f) { perror(path); return 1; }
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        size_t n = strlen(s);
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
        while (*s == ' ') s++;
        if (!strncmp(s, "---- ", 5)) {
            blocks = realloc(blocks, (size_t)(nblocks + 1) * sizeof *blocks);
            if (!blocks) exit(2);
            b = &blocks[nblocks++];
            memset(b, 0, sizeof *b);
            b->pc = strtoul(s + 5, NULL, 16);
            continue;
        }
        if (!strncmp(line, "OP:", 3) || !*s) {
            b = NULL;
            continue;
        }
        if (b)
            add_op(b, s);
    }
    fclose(f);
    return 0;
}

static int ident(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '+';
}

/* Does `op` match the pattern p[0..n)? `?` takes a run of name characters. */
static int match1(const char *p, int n, const char *op)
{
    int i = 0;
    while (i < n) {
        if (p[i] == '?') {
            if (!ident(*op))
                return 0;
            while (ident(*op)) op++;
            i++;
            continue;
        }
        if (p[i] != *op)
            return 0;
        i++; op++;
    }
    return *op == 0;
}

static int match(const char *pat, const char *op)
{
    const char *bar;
    while ((bar = strchr(pat, '|')) != NULL) {
        if (match1(pat, (int)(bar - pat), op))
            return 1;
        pat = bar + 1;
    }
    return match1(pat, (int)strlen(pat), op);
}

static int check(const char *path)
{
    int bad = 0;
    if (read_log(path))
        return 2;
    for (int t = 0; t < ntests; t++) {
        struct test *x = &tests[t];
        struct block *b = NULL;
        int at = 0;
        for (int k = 0; k < nblocks; k++)
            if (blocks[k].pc == x->pc) { b = &blocks[k]; break; }
        if (!b && x->optional)
            continue;
        if (!b) {
            if (bad++ < 20)
                printf("  0x%08lx %s: never translated (the walk went "
                       "elsewhere)\n", x->pc, x->text);
            continue;
        }
        for (int k = 0; k < b->nops; k++)
            if ((strstr(b->ops[k], "raise_exception") ||
                 strstr(b->ops[k], "qemu_excp")) &&
                !(x->nwant && strstr(x->want[0], "raise_exception"))) {
                if (bad++ < 20)
                    printf("  0x%08lx %s: QEMU raises an exception: %s\n",
                           x->pc, x->text, b->ops[k]);
                goto next;
            }
        for (int w = 0; w < x->nwant; w++) {
            while (at < b->nops && !match(x->want[w], b->ops[at]))
                at++;
            if (at == b->nops) {
                if (bad++ < 20) {
                    printf("  0x%08lx %s: no `%s` in QEMU's translation:\n",
                           x->pc, x->text, x->want[w]);
                    for (int k = 0; k < b->nops && k < 12; k++)
                        printf("      %s\n", b->ops[k]);
                }
                goto next;
            }
            at++;
        }
    next:;
    }
    if (bad) {
        printf("%d of %d instructions are not what QEMU decodes\n", bad,
               ntests);
        return 1;
    }
    printf("%d\n", ntests);
    return 0;
}

/* ---- range checks ---------------------------------------------------------
 *
 * Every one is load-bearing: the field is narrower than the C type that
 * carries it, and a missing check would silently truncate. */
static void refuse(int n)
{
    switch (n) {
    case 0:  tc_mov_imm(&C, 1, 32768); break;
    case 1:  tc_mov_imm(&C, 1, -32769); break;
    case 2:  tc_mov_u(&C, 1, -1); break;
    case 3:  tc_mov_u(&C, 1, 65536); break;
    case 4:  tc_addi(&C, 1, 2, 32768); break;
    case 5:  tc_lea(&C, 1, 2, -32769); break;
    case 6:  tc_load(&C, 1, 2, 32768, 4, 0); break;
    case 7:  tc_store(&C, 1, 2, -32769, 1); break;
    case 8:  tc_alu_imm(&C, TC_ADD, 1, 2, 256); break;
    case 9:  tc_alu_imm(&C, TC_ADD, 1, 2, -257); break;
    case 10: tc_alu_imm(&C, TC_AND, 1, 2, 512); break;
    case 11: tc_alu_imm(&C, TC_AND, 1, 2, -1); break;
    case 12: tc_alu_imm(&C, TC_LTU, 1, 2, -1); break;
    case 13: tc_alu_imm(&C, TC_SH, 1, 2, 32); break;
    case 14: tc_alu_imm(&C, TC_SH, 1, 2, -33); break;
    case 15: tc_alu_imm(&C, TC_SUB, 1, 2, 1); break;       /* no RC form */
    case 16: tc_enc_jcc(TC_JEQ, 1, 2, 32768); break;
    case 17: tc_enc_jcc(TC_JEQ, 1, 2, -32770); break;
    case 18: tc_enc_jcc(TC_JEQ, 1, 2, 3); break;            /* odd */
    case 19: tc_enc_jcci(TC_JEQ, 1, 8, 0); break;
    case 20: tc_enc_jcci(TC_JEQ, 1, -9, 0); break;
    case 21: tc_enc_jcci(TC_JLTU, 1, -1, 0); break;
    case 22: tc_enc_jcci(TC_JGEU, 1, 16, 0); break;
    case 23: tc_enc_j(16777216); break;
    case 24: tc_enc_call(-16777218); break;
    case 25: tc_mul64(&C, 3, 1, 2, 0); break;               /* odd pair */
    case 26: tc_div(&C, 5, 1, 2, 1); break;
    case 27: tc_extr(&C, 1, 2, 30, 3, 0); break;            /* past bit 31 */
    case 28: tc_extr(&C, 1, 2, 0, 0, 0); break;
    case 29: tc_insert_imm(&C, 1, 2, 16, 0, 4); break;
    case 30: tc_mov(&C, 16, 1); break;
    case 31: tc_mov_a(&C, 1, -1); break;
    case 32: tc_syscall(&C, 256); break;
    case 33: tc_dextr(&C, 1, 2, 3, 32); break;
    case 34: tc_movh(&C, 1, 0x10000); break;
    case 35: tc_addsc_a(&C, 1, 2, 3, 4); break;
    case 36: tc_swap_w(&C, 1, 2, 512); break;
    case 37: tc_cmpswap_w(&C, 1, 2, 0); break;            /* odd pair */
    case 38: tc_cmpswap_w(&C, 2, 2, -513); break;
    default: break;
    }
}
#define NREFUSE 39

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--image")) {
        build();
        return write_image(argv[2]);
    }
    if (argc > 2 && !strcmp(argv[1], "--check")) {
        build();
        return check(argv[2]);
    }
    if (argc > 1 && !strcmp(argv[1], "--refuse")) {
        if (argc > 2 && !strcmp(argv[2], "list")) {
            printf("%d\n", NREFUSE);
            return 0;
        }
        refuse(argc > 2 ? atoi(argv[2]) : -1);
        fprintf(stderr, "tricorecheck: range check %s did not fire\n",
                argc > 2 ? argv[2] : "?");
        return 0;
    }
    fprintf(stderr, "usage: tricorecheck --image FILE | --check LOG | "
                    "--refuse N|list\n");
    return 2;
}
