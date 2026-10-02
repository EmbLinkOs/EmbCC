/* Emits one instruction per call through src/arch/riscv/emit.c and writes
 * the raw bytes to stdout, alongside the disassembly EACH ONE IS SUPPOSED
 * TO BE on stderr. tests/golden/riscv-encoding.sh disassembles the bytes
 * with llvm-objdump and diffs the two.
 *
 * This is the only defence against a wrong bit in an encoding: a backend
 * that assembles its own instructions has no assembler to catch it. The
 * Thumb encoder shipped three wrong bits past careful reading and this
 * check found all three in one run.
 *
 * The expectations below are written in the DISASSEMBLER's spelling,
 * which for RISC-V means its pseudo-instructions: `addi rd, rs, 0` comes
 * back as `mv`, `jalr zero, 0(ra)` as `ret`, `xori rd, rs, -1` as `not`.
 * That is not a concession -- it is the check working. An encoder that
 * meant `mv` and produced something else would print as that something
 * else, and an alias only appears when the bits really are the aliased
 * instruction.
 *
 * `--li32` and `--li64` do not go through the disassembler at all. They
 * sweep constants through rv_li() and EXECUTE the sequence it emits with
 * the tiny interpreter at the bottom of this file, checking the register
 * ends up holding the value that was asked for. Disassembly could only
 * say that each instruction is the one intended; this says the sequence
 * computes the right number, which is the actual claim.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/riscv/emit.h"

static struct code C;

/* `text` is the expected disassembly; '|' separates one call's several
 * instructions. */
static void expect(const char *text)
{
    const char *p = text;
    for (;;) {
        const char *bar = strchr(p, '|');
        int n = bar ? (int)(bar - p) : (int)strlen(p);
        fprintf(stderr, "%.*s\n", n, p);
        if (!bar) break;
        p = bar + 1;
    }
}

/* ---- the instruction set, one call and one expectation at a time ----- */

static void encodings(int xlen)
{
    /* Moves and constants. */
    expect("mv a0, a1");                 rv_mv(&C, RV_A0, RV_A1);
    expect("lui a0, 1048575");           rv_lui(&C, RV_A0, 0xfffff);
    expect("auipc a0, 1");               rv_auipc(&C, RV_A0, 1);

    /* The register file by ABI name, so a wrong number in the enum shows
     * up as the wrong register rather than as nothing. */
    expect("mv ra, sp");                 rv_mv(&C, RV_RA, RV_SP);
    expect("mv gp, tp");                 rv_mv(&C, RV_GP, RV_TP);
    expect("mv t0, t1");                 rv_mv(&C, RV_T0, RV_T1);
    expect("mv t2, s0");                 rv_mv(&C, RV_T2, RV_FP);
    expect("mv s1, a0");                 rv_mv(&C, RV_S1, RV_A0);
    expect("mv a2, a3");                 rv_mv(&C, RV_A2, RV_A3);
    expect("mv a4, a5");                 rv_mv(&C, RV_A4, RV_A5);
    expect("mv a6, a7");                 rv_mv(&C, RV_A6, RV_A7);
    expect("mv s2, t3");                 rv_mv(&C, RV_S2, RV_T3);
    expect("mv t4, t5");                 rv_mv(&C, RV_T4, RV_T5);
    /* `addi t6, zero, 0` is `mv t6, zero` and `li t6, 0` alike, and
     * the disassembler prefers the second. Same instruction. */
    expect("li t6, 0");                  rv_mv(&C, RV_T6, RV_ZERO);

    /* The ten register-register operations. */
    expect("add a0, a1, a2");    rv_alu(&C, RV_ADD,  RV_A0, RV_A1, RV_A2, 0);
    expect("sub a0, a1, a2");    rv_alu(&C, RV_SUB,  RV_A0, RV_A1, RV_A2, 0);
    expect("sll a0, a1, a2");    rv_alu(&C, RV_SLL,  RV_A0, RV_A1, RV_A2, 0);
    expect("slt a0, a1, a2");    rv_alu(&C, RV_SLT,  RV_A0, RV_A1, RV_A2, 0);
    expect("sltu a0, a1, a2");   rv_alu(&C, RV_SLTU, RV_A0, RV_A1, RV_A2, 0);
    expect("xor a0, a1, a2");    rv_alu(&C, RV_XOR,  RV_A0, RV_A1, RV_A2, 0);
    expect("srl a0, a1, a2");    rv_alu(&C, RV_SRL,  RV_A0, RV_A1, RV_A2, 0);
    expect("sra a0, a1, a2");    rv_alu(&C, RV_SRA,  RV_A0, RV_A1, RV_A2, 0);
    expect("or a0, a1, a2");     rv_alu(&C, RV_OR,   RV_A0, RV_A1, RV_A2, 0);
    expect("and a0, a1, a2");    rv_alu(&C, RV_AND,  RV_A0, RV_A1, RV_A2, 0);

    /* The immediate forms, at both ends of the 12-bit signed field. */
    expect("addi a0, a1, -2048"); rv_alu_imm(&C, RV_ADD,  RV_A0, RV_A1, -2048, 0);
    expect("addi a0, a1, 2047");  rv_alu_imm(&C, RV_ADD,  RV_A0, RV_A1, 2047, 0);
    expect("slti a0, a1, 7");     rv_alu_imm(&C, RV_SLT,  RV_A0, RV_A1, 7, 0);
    expect("sltiu a0, a1, 7");    rv_alu_imm(&C, RV_SLTU, RV_A0, RV_A1, 7, 0);
    expect("xori a0, a1, 255");   rv_alu_imm(&C, RV_XOR,  RV_A0, RV_A1, 255, 0);
    expect("not a0, a1");         rv_alu_imm(&C, RV_XOR,  RV_A0, RV_A1, -1, 0);
    expect("ori a0, a1, 1");      rv_alu_imm(&C, RV_OR,   RV_A0, RV_A1, 1, 0);
    expect("andi a0, a1, 15");    rv_alu_imm(&C, RV_AND,  RV_A0, RV_A1, 15, 0);
    /* `andi rd, rs, 255` IS the zero-extend-byte idiom and prints as
     * its alias. Kept because that is the form the codegen will emit
     * for a cast to unsigned char. */
    expect("zext.b a0, a1");      rv_alu_imm(&C, RV_AND,  RV_A0, RV_A1, 255, 0);

    /* Shifts by an immediate: the operand is an AMOUNT, and its width is
     * what distinguishes RV32 from RV64 here. 31 is the largest at RV32
     * and 63 at RV64; rv_shift_imm refuses the one that does not belong,
     * which is checked by --shifts rather than here. */
    expect("slli a0, a1, 31");  rv_shift_imm(&C, RV_SLL, RV_A0, RV_A1, 31, 0, xlen);
    expect("srli a0, a1, 1");   rv_shift_imm(&C, RV_SRL, RV_A0, RV_A1, 1, 0, xlen);
    expect("srai a0, a1, 31");  rv_shift_imm(&C, RV_SRA, RV_A0, RV_A1, 31, 0, xlen);
    if (xlen == 64) {
        expect("slli a0, a1, 63"); rv_shift_imm(&C, RV_SLL, RV_A0, RV_A1, 63, 0, 64);
        expect("srli a0, a1, 63"); rv_shift_imm(&C, RV_SRL, RV_A0, RV_A1, 63, 0, 64);
        expect("srai a0, a1, 63"); rv_shift_imm(&C, RV_SRA, RV_A0, RV_A1, 63, 0, 64);
    }

    /* The M extension. */
    expect("mul a0, a1, a2");    rv_muldiv(&C, RV_MUL,    RV_A0, RV_A1, RV_A2, 0);
    expect("mulh a0, a1, a2");   rv_muldiv(&C, RV_MULH,   RV_A0, RV_A1, RV_A2, 0);
    expect("mulhsu a0, a1, a2"); rv_muldiv(&C, RV_MULHSU, RV_A0, RV_A1, RV_A2, 0);
    expect("mulhu a0, a1, a2");  rv_muldiv(&C, RV_MULHU,  RV_A0, RV_A1, RV_A2, 0);
    expect("div a0, a1, a2");    rv_muldiv(&C, RV_DIV,    RV_A0, RV_A1, RV_A2, 0);
    expect("divu a0, a1, a2");   rv_muldiv(&C, RV_DIVU,   RV_A0, RV_A1, RV_A2, 0);
    expect("rem a0, a1, a2");    rv_muldiv(&C, RV_REM,    RV_A0, RV_A1, RV_A2, 0);
    expect("remu a0, a1, a2");   rv_muldiv(&C, RV_REMU,   RV_A0, RV_A1, RV_A2, 0);

    /* The RV64 word forms: a different opcode and otherwise the same
     * fields, which is why `w` is a flag and not a second table. */
    if (xlen == 64) {
        expect("addw a0, a1, a2");  rv_alu(&C, RV_ADD, RV_A0, RV_A1, RV_A2, 1);
        expect("subw a0, a1, a2");  rv_alu(&C, RV_SUB, RV_A0, RV_A1, RV_A2, 1);
        expect("sllw a0, a1, a2");  rv_alu(&C, RV_SLL, RV_A0, RV_A1, RV_A2, 1);
        expect("srlw a0, a1, a2");  rv_alu(&C, RV_SRL, RV_A0, RV_A1, RV_A2, 1);
        expect("sraw a0, a1, a2");  rv_alu(&C, RV_SRA, RV_A0, RV_A1, RV_A2, 1);
        expect("addiw a0, a1, 7");  rv_alu_imm(&C, RV_ADD, RV_A0, RV_A1, 7, 1);
        expect("slliw a0, a1, 31"); rv_shift_imm(&C, RV_SLL, RV_A0, RV_A1, 31, 1, 64);
        expect("srliw a0, a1, 31"); rv_shift_imm(&C, RV_SRL, RV_A0, RV_A1, 31, 1, 64);
        expect("sraiw a0, a1, 31"); rv_shift_imm(&C, RV_SRA, RV_A0, RV_A1, 31, 1, 64);
        expect("mulw a0, a1, a2");  rv_muldiv(&C, RV_MUL,  RV_A0, RV_A1, RV_A2, 1);
        expect("divw a0, a1, a2");  rv_muldiv(&C, RV_DIV,  RV_A0, RV_A1, RV_A2, 1);
        expect("divuw a0, a1, a2"); rv_muldiv(&C, RV_DIVU, RV_A0, RV_A1, RV_A2, 1);
        expect("remw a0, a1, a2");  rv_muldiv(&C, RV_REM,  RV_A0, RV_A1, RV_A2, 1);
        expect("remuw a0, a1, a2"); rv_muldiv(&C, RV_REMU, RV_A0, RV_A1, RV_A2, 1);
    }

    /* Loads and stores. The negative offset is the one that matters: the
     * S-type immediate is split across two fields and a sign bit put in
     * the wrong one gives a store 4064 bytes away that still assembles. */
    expect("lb a0, 4(sp)");    rv_load(&C, RV_A0, RV_SP, 4, 1, 1, xlen);
    expect("lbu a0, 4(sp)");   rv_load(&C, RV_A0, RV_SP, 4, 1, 0, xlen);
    expect("lh a0, 4(sp)");    rv_load(&C, RV_A0, RV_SP, 4, 2, 1, xlen);
    expect("lhu a0, 4(sp)");   rv_load(&C, RV_A0, RV_SP, 4, 2, 0, xlen);
    expect("lw a0, -4(sp)");   rv_load(&C, RV_A0, RV_SP, -4, 4, 1, xlen);
    expect("lw a0, 2047(sp)"); rv_load(&C, RV_A0, RV_SP, 2047, 4, 1, xlen);
    expect("lw a0, -2048(sp)"); rv_load(&C, RV_A0, RV_SP, -2048, 4, 1, xlen);
    expect("sb a0, 4(sp)");    rv_store(&C, RV_A0, RV_SP, 4, 1, xlen);
    expect("sh a0, 4(sp)");    rv_store(&C, RV_A0, RV_SP, 4, 2, xlen);
    expect("sw a0, -4(sp)");   rv_store(&C, RV_A0, RV_SP, -4, 4, xlen);
    expect("sw a0, 2047(sp)"); rv_store(&C, RV_A0, RV_SP, 2047, 4, xlen);
    expect("sw a0, -2048(sp)"); rv_store(&C, RV_A0, RV_SP, -2048, 4, xlen);
    if (xlen == 64) {
        expect("lwu a0, 4(sp)");  rv_load(&C, RV_A0, RV_SP, 4, 4, 0, xlen);
        expect("ld a0, 8(sp)");   rv_load(&C, RV_A0, RV_SP, 8, 8, 1, xlen);
        expect("ld a0, -8(sp)");  rv_load(&C, RV_A0, RV_SP, -8, 8, 1, xlen);
        expect("sd a0, 8(sp)");   rv_store(&C, RV_A0, RV_SP, 8, 8, xlen);
        expect("sd a0, -8(sp)");  rv_store(&C, RV_A0, RV_SP, -8, 8, xlen);
    }

    /* Control flow. The placeholders are emitted and then patched to a
     * known distance, so what is checked is the PATCH -- which is the
     * part with the scrambled immediate and the part a codegen depends on
     * being right for every branch it ever emits. */
    {
        int at;
        /* The disassembler has no addresses here, so it prints the raw
         * signed displacement -- which is exactly the number rv_patch_b
         * was given. Both extremes of each field are covered: a B-type
         * reaches -4096..+4094 and a J-type -1048576..+1048574. */
        expect("beq a0, a1, 8");      at = rv_b_placeholder(&C, RV_BEQ, RV_A0, RV_A1);
        rv_patch_b(&C, at, at + 8);
        expect("bne a0, a1, 4");      at = rv_b_placeholder(&C, RV_BNE, RV_A0, RV_A1);
        rv_patch_b(&C, at, at + 4);
        expect("blt a0, a1, -4");     at = rv_b_placeholder(&C, RV_BLT, RV_A0, RV_A1);
        rv_patch_b(&C, at, at - 4);
        expect("bge a0, a1, -2048");  at = rv_b_placeholder(&C, RV_BGE, RV_A0, RV_A1);
        rv_patch_b(&C, at, at - 2048);
        expect("bltu a0, a1, 4094");  at = rv_b_placeholder(&C, RV_BLTU, RV_A0, RV_A1);
        rv_patch_b(&C, at, at + 4094);
        expect("bgeu a0, a1, -4096"); at = rv_b_placeholder(&C, RV_BGEU, RV_A0, RV_A1);
        rv_patch_b(&C, at, at - 4096);

        expect("j 1048574");          at = rv_j_placeholder(&C, RV_ZERO);
        rv_patch_j(&C, at, at + 1048574);
        expect("jal -1048576");       at = rv_j_placeholder(&C, RV_RA);
        rv_patch_j(&C, at, at - 1048576);
    }
    expect("jalr a0");           rv_jalr(&C, RV_RA, RV_A0, 0);
    expect("jalr 4(a0)");        rv_jalr(&C, RV_RA, RV_A0, 4);
    expect("ret");               rv_ret(&C);
    expect("auipc ra, 0|jalr ra"); rv_call_placeholder(&C);

    expect("unimp");             rv_unimp(&C);
    expect("ebreak");            rv_ebreak(&C);
}

/* ---- rv_li, checked by EXECUTING it ---------------------------------- */

/* Just enough of the machine to run what li_emit emits: lui, addi, addiw
 * and slli. Decoded from the bytes, so a wrong field here fails the same
 * way a wrong field in a real program would. */
static long long run_li(const unsigned char *p, int n, int rd, int xlen)
{
    long long x[32];
    for (int i = 0; i < 32; i++) x[i] = 0;
    for (int i = 0; i < n; i += 4) {
        unsigned long w = (unsigned long)p[i] | ((unsigned long)p[i+1] << 8) |
                          ((unsigned long)p[i+2] << 16) |
                          ((unsigned long)p[i+3] << 24);
        int op = (int)(w & 0x7f), d = (int)((w >> 7) & 0x1f);
        int f3 = (int)((w >> 12) & 7), s1 = (int)((w >> 15) & 0x1f);
        long long imm = (long long)(int)((w >> 20) & 0xfff);
        if (imm & 0x800) imm -= 0x1000;                 /* sign-extend */
        switch (op) {
        case 0x37: {                                    /* lui */
            long long v = (long long)(int)(unsigned int)((w & 0xfffff000UL));
            x[d] = v;
            break;
        }
        case 0x13:
            if (f3 == 0) x[d] = x[s1] + imm;            /* addi */
            else if (f3 == 1) x[d] = (long long)((unsigned long long)x[s1]
                                                 << ((w >> 20) & 0x3f));
            else { fprintf(stderr, "li: funct3 %d\n", f3); exit(1); }
            break;
        case 0x1b:                                      /* addiw */
            x[d] = (long long)(int)(unsigned int)(x[s1] + imm);
            break;
        default:
            fprintf(stderr, "li: opcode 0x%02x is not one rv_li emits\n", op);
            exit(1);
        }
        if (xlen == 32)
            x[d] = (long long)(int)(unsigned int)x[d];  /* a 32-bit register */
    }
    return x[rd];
}

static int li_one(long long v, int xlen, int *nbad)
{
    struct code c = { 0 };
    rv_li(&c, RV_A0, v, xlen);
    long long want = v;
    if (xlen == 32) want = (long long)(int)(unsigned int)v;
    long long got = run_li(c.p, c.len, RV_A0, xlen);
    int len = rv_li_len(v, xlen);
    if (got != want) {
        printf("li %lld (rv%d): the sequence computes %lld\n", v, xlen, got);
        (*nbad)++;
    }
    if (len != c.len) {
        printf("li %lld (rv%d): rv_li_len says %d, emitted %d\n",
               v, xlen, len, c.len);
        (*nbad)++;
    }
    free(c.p);
    return 1;
}

static int sweep_li(int xlen)
{
    static const long long edge[] = {
        0, 1, -1, 2, -2, 2047, 2048, -2048, -2049, 4095, 4096, -4096, -4097,
        0x7ff, 0x800, 0xfff, 0x1000, 0xffff, 0x10000, 0x7ffff, 0x80000,
        0x7ffff800LL, 0x7ffff7ffLL, 0x7fffffffLL, -0x80000000LL,
        0x80000000LL, 0xfffffffLL, 0x12345678LL, -0x12345678LL
    };
    static const long long edge64[] = {
        0x100000000LL, 0x1122334455667788LL, -0x1122334455667788LL,
        0x7fffffffffffffffLL, (-0x7fffffffffffffffLL - 1),
        0xdeadbeefcafeLL, 0x800000000LL, 0xffffffff80000000LL,
        0x00000000ffffffffLL, 0x0000000100000001LL
    };
    int n = 0, bad = 0;
    for (size_t i = 0; i < sizeof edge / sizeof *edge; i++) {
        long long v = edge[i];
        if (xlen == 32 && (v < -2147483648LL || v > 4294967295LL)) continue;
        n += li_one(v, xlen, &bad);
    }
    if (xlen == 64)
        for (size_t i = 0; i < sizeof edge64 / sizeof *edge64; i++)
            n += li_one(edge64[i], xlen, &bad);

    /* And a deterministic pseudo-random spread, because the edges are the
     * values the author already thought about. */
    unsigned long long s = 0x243f6a8885a308d3ULL;
    for (int i = 0; i < 20000; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        long long v = (long long)s;
        if (xlen == 32) v = (long long)(int)(unsigned int)s;
        n += li_one(v, xlen, &bad);
    }
    if (bad) {
        printf("rv%d: %d of %d rv_li sequences are wrong\n", xlen, bad, n);
        return 1;
    }
    printf("rv%d: %d rv_li sequences each compute the value asked for\n",
           xlen, n);
    return 0;
}

/* ---- the refusals ----------------------------------------------------- */

/* Every range check in the encoder is load-bearing: the field it guards is
 * narrower than the C type the caller passes, and a truncated value
 * assembles into a real instruction pointing somewhere else. Each one is
 * provoked here by number, and tests/golden/riscv-encoding.sh checks the
 * process dies rather than emitting. A check that never fires is a check
 * nobody has seen work.
 *
 * `--refuse list` prints how many there are, so adding one without adding
 * its case fails the test rather than going unnoticed. */
#define NREFUSE 12
static void refuse(int n)
{
    struct code c = { 0 };
    switch (n) {
    case 0: rv_shift_imm(&c, RV_SLL, RV_A0, RV_A1, 32, 0, 32); break;
    case 1: rv_shift_imm(&c, RV_SLL, RV_A0, RV_A1, 64, 0, 64); break;
    case 2: rv_shift_imm(&c, RV_SLL, RV_A0, RV_A1, 32, 1, 64); break;
    case 3: rv_alu_imm(&c, RV_ADD, RV_A0, RV_A1, 2048, 0); break;
    case 4: rv_store(&c, RV_A0, RV_SP, -2049, 4, 32); break;
    case 5: rv_alu_imm(&c, RV_SUB, RV_A0, RV_A1, 1, 0); break;
    case 6: rv_alu_imm(&c, RV_SLL, RV_A0, RV_A1, 1, 0); break;
    case 7: rv_alu(&c, RV_AND, RV_A0, RV_A1, RV_A2, 1); break;
    case 8: rv_li(&c, RV_A0, 0x100000000LL, 32); break;
    case 9: rv_load(&c, RV_A0, RV_SP, 0, 3, 1, 32); break;
    /* `ld`/`sd` and `lwu` are RV64-only, and at RV32 they assemble into
     * real 32-bit words that trap as illegal instructions the first time
     * a struct argument is copied -- which is how it was found. */
    case 10: rv_load(&c, RV_A0, RV_SP, 0, 8, 1, 32); break;
    case 11: rv_store(&c, RV_A0, RV_SP, 0, 8, 32); break;
    default: printf("no refusal %d\n", n); exit(2);
    }
    printf("refusal %d did not fire; %d bytes were emitted\n", n, c.len);
    exit(1);
}

/* --csweep32 / --csweep64: the compressed forms over a WIDE operand
 * space, not just the instructions the corpus above happens to use.
 *
 * The bit layouts that are easy to get wrong are the scattered
 * immediates -- c.lw's offset is bits 5:3 and 2 and 6 in three
 * separate places, c.addi16sp's is in five -- and a corpus of a
 * hundred instructions exercises almost none of that space. So: walk
 * registers and offsets across their boundaries, write the 32-bit
 * words as hex on stderr and the compressed bytes on stdout, and let
 * the shell round-trip the hex through llvm-mc (disassemble without
 * +c, reassemble WITH it, which compresses on its own) and compare.
 *
 * No expectation text is written by hand anywhere in this mode, so
 * the corpus can be as large as it likes. */
static void csweep_one(unsigned long w, int xlen)
{
    unsigned c = rv_compress(w, xlen);
    fprintf(stderr, "0x%02lx 0x%02lx 0x%02lx 0x%02lx\n",
            w & 0xff, (w >> 8) & 0xff, (w >> 16) & 0xff, (w >> 24) & 0xff);
    if (c) {
        putchar((int)(c & 0xff));
        putchar((int)((c >> 8) & 0xff));
    } else {
        for (int k = 0; k < 4; k++)
            putchar((int)((w >> (8 * k)) & 0xff));
    }
}

static int csweep(int xlen)
{
    static const int regs[] = { 0, 1, 2, 5, 7, 8, 9, 10, 13, 15, 16, 28, 31 };
    static const int imms[] = { -2048, -513, -512, -33, -32, -16, -1, 0, 1,
                                4, 15, 16, 31, 32, 63, 255, 256, 496, 2047 };
    const int NR = (int)(sizeof regs / sizeof regs[0]);
    const int NI = (int)(sizeof imms / sizeof imms[0]);

    for (int a = 0; a < NR; a++)
        for (int b = 0; b < NR; b++)
            for (int i = 0; i < NI; i++) {
                int rd = regs[a], rs = regs[b], im = imms[i];
                /* addi / andi / slli / srli / srai, and addiw at RV64 */
                csweep_one(rv_enc_i(0x13, rd, 0, rs, im), xlen);
                csweep_one(rv_enc_i(0x13, rd, 7, rs, im), xlen);
                if (xlen == 64)
                    csweep_one(rv_enc_i(0x1b, rd, 0, rs, im), xlen);
                /* Shifts are built by hand rather than through
                 * rv_enc_r: at RV64 a shift amount reaches 63 and the
                 * field it sits in is six bits, which the register
                 * packer correctly refuses to be handed. */
                int sh = im & (xlen == 64 ? 0x3f : 0x1f);
                unsigned long base = 0x13u | ((unsigned long)rd << 7) |
                                     ((unsigned long)rs << 15) |
                                     ((unsigned long)sh << 20);
                csweep_one(base | (1UL << 12), xlen);               /* slli */
                csweep_one(base | (5UL << 12), xlen);               /* srli */
                csweep_one(base | (5UL << 12) | (0x20UL << 25), xlen); /* srai */
                /* loads and stores, where the scattered offsets live */
                if (im >= 0) {
                    csweep_one(rv_enc_i(0x03, rd, 2, rs, im), xlen);
                    csweep_one(rv_enc_s(0x23, 2, rs, rd, im), xlen);
                    if (xlen == 64) {
                        csweep_one(rv_enc_i(0x03, rd, 3, rs, im), xlen);
                        csweep_one(rv_enc_s(0x23, 3, rs, rd, im), xlen);
                    }
                }
                /* jalr with a zero displacement (c.jr / c.jalr / ret) */
                if (im == 0)
                    csweep_one(rv_enc_i(0x67, rd, 0, rs, 0), xlen);
            }
    /* the register-register forms, and lui */
    for (int a = 0; a < NR; a++)
        for (int b = 0; b < NR; b++) {
            int rd = regs[a], rs2 = regs[b];
            csweep_one(rv_enc_r(0x33, rd, 0, rd, rs2, 0), xlen);      /* add */
            csweep_one(rv_enc_r(0x33, rd, 0, 0, rs2, 0), xlen);       /* mv-ish */
            csweep_one(rv_enc_r(0x33, rd, 0, rd, rs2, 0x20), xlen);   /* sub */
            csweep_one(rv_enc_r(0x33, rd, 4, rd, rs2, 0), xlen);      /* xor */
            csweep_one(rv_enc_r(0x33, rd, 6, rd, rs2, 0), xlen);      /* or */
            csweep_one(rv_enc_r(0x33, rd, 7, rd, rs2, 0), xlen);      /* and */
            if (xlen == 64) {
                csweep_one(rv_enc_r(0x3b, rd, 0, rd, rs2, 0), xlen);  /* addw */
                csweep_one(rv_enc_r(0x3b, rd, 0, rd, rs2, 0x20), xlen); /* subw */
            }
        }
    for (int a = 0; a < NR; a++)
        for (int i = 0; i < NI; i++)
            csweep_one(rv_enc_u(0x37, regs[a], imms[i] & 0xfffff), xlen);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "--rv32";
    if (strcmp(mode, "--csweep32") == 0) return csweep(32);
    if (strcmp(mode, "--csweep64") == 0) return csweep(64);
    if (strcmp(mode, "--li32") == 0) return sweep_li(32);
    if (strcmp(mode, "--li64") == 0) return sweep_li(64);
    if (strcmp(mode, "--refuse") == 0) {
        if (argc > 2 && strcmp(argv[2], "list") == 0) {
            printf("%d\n", NREFUSE);
            return 0;
        }
        refuse(argc > 2 ? atoi(argv[2]) : -1);
    }
    /* --c32 / --c64: the SAME instruction sweep, but every 32-bit word
     * is run through rv_compress() and the short form written when
     * there is one. The shell then assembles the same expectation text
     * with `llvm-mc -mattr=+c`, which compresses on its own, and the
     * two byte streams must be identical.
     *
     * That is the referee the compressed forms need and the only one
     * worth having: it compares against an assembler's opinion of what
     * the SAME instruction compresses to, over the whole corpus, so a
     * wrong bit in a field cannot hide behind a form that is never
     * reached. */
    int cmode = strcmp(mode, "--c32") == 0 || strcmp(mode, "--c64") == 0;
    int xlen = (strcmp(mode, "--rv64") == 0 || strcmp(mode, "--c64") == 0)
               ? 64 : 32;
    encodings(xlen);
    if (!cmode) {
        fwrite(C.p, 1, (size_t)C.len, stdout);
        return 0;
    }
    /* One hex line per instruction, in the same order as the
     * expectations on stderr, so a mismatch names the instruction
     * instead of an offset into a stream whose lengths have already
     * diverged. */
    for (int i = 0; i + 3 < C.len; i += 4) {
        unsigned long w = (unsigned long)C.p[i] |
                          ((unsigned long)C.p[i + 1] << 8) |
                          ((unsigned long)C.p[i + 2] << 16) |
                          ((unsigned long)C.p[i + 3] << 24);
        unsigned c = rv_compress(w, xlen);
        if (c)
            printf("%02x%02x\n", c & 0xff, (c >> 8) & 0xff);
        else
            printf("%02x%02x%02x%02x\n", (unsigned)C.p[i],
                   (unsigned)C.p[i + 1], (unsigned)C.p[i + 2],
                   (unsigned)C.p[i + 3]);
    }
    return 0;
}
