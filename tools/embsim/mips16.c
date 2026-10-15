/* mips16.c -- MIPS16e, the compressed instruction set of the 24Kc
 * (`24kc`, QEMU malta's) and of a PIC32MX's M4K core.
 *
 * An instruction is a halfword; EXTEND (major opcode 0x1e) makes the one
 * after it take a 16-bit immediate (a 15-bit one for addiu ry, rx, imm)
 * and a 5-bit shift, and jal and jalx are 32 bits. Eight registers are
 * reachable in three bits (s0, s1, v0, v1, a0-a3), t8 holds the
 * compares' results, and sp, ra and the move instructions reach the
 * rest. The branches -- b, beqz, bnez, bteqz, btnez, and jrc and jalrc --
 * have no delay slot; jal, jalx, jr and jalr have a 16-bit one. A link
 * register gets bit 0 set, for the ISA mode to return to; jalx switches
 * to MIPS32, and jr and jalr to an even address do.
 *
 * Also: save and restore (the extended forms with their argument and
 * static registers and the extra s registers), addiupc and lwpc (from
 * the instruction's word-aligned address), the sign and zero extensions
 * (seb, seh, zeb, zeh), li and cmpi with their zero-extended immediates.
 * As QEMU has it (the referee tests/golden/embsim-mips.sh holds this file
 * to): a branch in a delay slot, and an extended addiupc or lwpc in one,
 * are Reserved Instructions, and so is an instruction EXTEND cannot
 * extend. The 64-bit instructions are Reserved Instructions, as on a
 * MIPS32 core. */
#include "mips.h"

static const u8 gpr3[8] = { 16, 17, 2, 3, 4, 5, 6, 7 };

static u32 R(u32 n) { return ms->r[n]; }
static void W(u32 n, u32 v) { if (n) ms->r[n] = v; }

static void reserved(void)
{
    mips_raise(EXC_RI);
}

/* a MIPS32 load's or store's fields: base, rt, offset */
static u32 ls(u32 base, u32 rt, u32 off)
{
    return base << 21 | rt << 16 | (off & 0xffff);
}

static u32 sext(u32 v, int bits)
{
    return (u32)((s32)(v << (32 - bits)) >> (32 - bits));
}

int mips16_size(u32 first)
{
    u32 op = (first >> 11) & 0x1f;
    return op == 0x1e || op == 0x03 ? 4 : 2;
}

int mips16_cost(u32 i, int size)
{
    u32 h = size == 4 && ((i >> 27) & 0x1f) == 0x1e ? i & 0xffff : i >> (size == 4 ? 16 : 0);
    u32 op = (h >> 11) & 0x1f;
    if ((op >= 0x10 && op <= 0x16) || op == 0x12)              /* loads */
        return 2;
    if (op == 0x1d) {
        u32 f = h & 0x1f;
        if (f == 0x1a || f == 0x1b)                             /* div, divu */
            return 18;
        if (f == 0x18 || f == 0x19)                             /* mult, multu */
            return 2;
    }
    return 1;
}

/* A transfer: to `target` (the ISA bit in bit 0) if taken, after a 16-bit
 * delay slot (`slot`), or at once; `link` gets the address past the slot,
 * or past this instruction, with the bit set */
static void jump(int taken, u32 target, int size, int slot, u32 link)
{
    u32 pc = ms->pc;
    if (ms->slot) {                 /* a branch in a delay slot */
        reserved();
        return;
    }
    if (link)
        W(link, pc + (u32)size + (slot ? 2u : 0u) + 1);
    if (!slot) {
        if (taken)
            ms->nx = target;
        return;
    }
    if (taken)
        ms->nnx = target;
    ms->nslot = 1;
    ms->nbpc = pc;
    ms->nds = 0;
    ms->nbtaken = taken;
}

/* b to itself: an idle loop (no slot to look at) */
static void branch_always(u32 target, int size)
{
    if (target == (ms->pc | 1))
        mips_self_loop(target, 1);
    jump(1, target, size, 0, 0);
}

/* addiupc's and lwpc's base: the instruction's word, or its delay slot's
 * branch's */
static u32 pcrel(void)
{
    return (ms->slot ? ms->bpc : ms->pc) & ~3u;
}

/* save and restore: aregs's argument registers (stored in the caller's
 * frame) and static ones (in this frame), the extra s registers, s0, s1
 * and ra, and the frame */
static void save_restore(int save, u32 xsregs, u32 aregs, int ra, int s0, int s1,
                         u32 frame)
{
    static const int8_t nargs[16] = { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 0, 3, 3, 4, -1 };
    static const int8_t nstatic[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 4, 0, 1, 0, -1 };
    static const u8 xs[7] = { 30, 23, 22, 21, 20, 19, 18 };
    int args = nargs[aregs], st = nstatic[aregs];
    u32 sp = R(29), a;
    if (args < 0) {
        reserved();
        return;
    }
    if (save)
        for (int k = args - 1; k >= 0 && !ms->exc; k--)
            mips_store(sp + 4 * (u32)k, 4, R(4 + (u32)k));
    a = save ? sp : sp + frame;
    /* each one at the word below the last: ra, the extra s registers
     * from the highest, s1, s0, then the static a registers from a3 */
    u32 regs[16];
    int n = 0;
    if (ra)
        regs[n++] = 31;
    for (u32 k = 7 - xsregs; k < 7; k++)
        regs[n++] = xs[k];
    if (s1)
        regs[n++] = 17;
    if (s0)
        regs[n++] = 16;
    for (int k = 0; k < st; k++)
        regs[n++] = 7 - (u32)k;
    for (int k = 0; k < n && !ms->exc; k++) {
        a -= 4;
        if (save) {
            mips_store(a, 4, R(regs[k]));
        } else {
            u32 v = mips_load(a, 4);
            if (!ms->exc)
                W(regs[k], v);
        }
    }
    if (!ms->exc)
        W(29, save ? sp - frame : sp + frame);
}

/* ---- the 16-bit instructions, and their extended forms ----------------- */

static void exec(u32 h, int ext, u32 e)
{
    u32 op = (h >> 11) & 0x1f, rx = gpr3[(h >> 8) & 7], ry = gpr3[(h >> 5) & 7];
    u32 pc = ms->pc, size = ext ? 4 : 2;
    /* the extended immediate: 16 bits, from EXTEND's two fields and the
     * instruction's low five */
    u32 imm = ext ? sext((e & 0x1f) << 11 | ((e >> 5) & 0x3f) << 5 | (h & 0x1f), 16) : 0;
    switch (op) {
    case 0x00:                                                  /* ADDIU rx, sp, imm */
        W(rx, R(29) + (ext ? imm : (h & 0xff) << 2));
        return;
    case 0x01:                                                  /* ADDIUPC */
        if (ext && ms->slot) {
            reserved();
            return;
        }
        W(rx, pcrel() + (ext ? imm : (h & 0xff) << 2));
        return;
    case 0x02: {                                                /* B */
        u32 off = ext ? imm << 1 : sext(h, 11) << 1;
        branch_always((pc + size + off) | 1, (int)size);
        return;
    }
    case 0x04: case 0x05: {                                     /* BEQZ, BNEZ */
        u32 off = ext ? imm << 1 : sext(h, 8) << 1;
        int z = R(rx) == 0;
        jump(op == 0x04 ? z : !z, (pc + size + off) | 1, (int)size, 0, 0);
        return;
    }
    case 0x06: {                                                /* SLL, SRL, SRA */
        u32 sa = ext ? (e >> 6) & 0x1f : ((h >> 2) & 7) ? (h >> 2) & 7 : 8;
        u32 v = R(ry);
        switch (h & 3) {
        case 0: W(rx, v << sa); return;
        case 2: W(rx, v >> sa); return;
        case 3: W(rx, (u32)((s32)v >> sa)); return;
        }
        break;                                                  /* DSLL */
    }
    case 0x08:                                                  /* ADDIU ry, rx, imm */
        if (h & 0x10)
            break;                                              /* DADDIU */
        W(ry, R(rx) + (ext ? sext((h & 0xf) | ((e >> 4) & 0x7f) << 4 |
                                  (e & 0xf) << 11, 15)
                           : sext(h, 4)));
        return;
    case 0x09:                                                  /* ADDIU rx, imm */
        W(rx, R(rx) + (ext ? imm : sext(h, 8)));
        return;
    case 0x0a:                                                  /* SLTI */
        W(24, (s32)R(rx) < (s32)(ext ? imm : h & 0xff));
        return;
    case 0x0b:                                                  /* SLTIU */
        W(24, R(rx) < (ext ? imm : h & 0xff));
        return;
    case 0x0c: {                                                /* I8 */
        switch ((h >> 8) & 7) {
        case 0: case 1: {                                       /* BTEQZ, BTNEZ */
            u32 off = ext ? imm << 1 : sext(h, 8) << 1;
            int z = R(24) == 0;
            jump(((h >> 8) & 7) == 0 ? z : !z, (pc + size + off) | 1, (int)size, 0, 0);
            return;
        }
        case 2:                                                 /* SW ra, imm(sp) */
            mips_store(R(29) + (ext ? imm : (h & 0xff) << 2), 4, R(31));
            return;
        case 3:                                                 /* ADJSP */
            W(29, R(29) + (ext ? imm : sext(h, 8) << 3));
            return;
        case 4: {                                               /* SAVE, RESTORE */
            u32 frame = ext ? (((e >> 4) & 0xf) << 4 | (h & 0xf)) << 3
                            : (h & 0xf) ? (h & 0xf) << 3 : 128;
            save_restore((h >> 7) & 1, ext ? (e >> 8) & 7 : 0, ext ? e & 0xf : 0,
                         (h >> 6) & 1, (h >> 5) & 1, (h >> 4) & 1, frame);
            return;
        }
        case 5:                                                 /* MOV32R */
            if (ext)
                break;
            W(((h >> 3) & 3) << 3 | ((h >> 5) & 7), R(gpr3[h & 7]));
            return;
        case 7:                                                 /* MOVR32 */
            if (ext)
                break;
            W(ry, R(h & 0x1f));
            return;
        }
        break;
    }
    case 0x0d:                                                  /* LI */
        W(rx, ext ? imm & 0xffff : h & 0xff);
        return;
    case 0x0e:                                                  /* CMPI */
        W(24, R(rx) ^ (ext ? imm & 0xffff : h & 0xff));
        return;
    case 0x10: mips_exec32(0x80000000u | ls(rx, ry, ext ? imm : h & 0x1f)); return;         /* LB */
    case 0x11: mips_exec32(0x84000000u | ls(rx, ry, ext ? imm : (h & 0x1f) << 1)); return;  /* LH */
    case 0x12:                                                  /* LW rx, imm(sp) */
        mips_exec32(0x8c000000u | ls(29, rx, ext ? imm : (h & 0xff) << 2));
        return;
    case 0x13: mips_exec32(0x8c000000u | ls(rx, ry, ext ? imm : (h & 0x1f) << 2)); return;  /* LW */
    case 0x14: mips_exec32(0x90000000u | ls(rx, ry, ext ? imm : h & 0x1f)); return;         /* LBU */
    case 0x15: mips_exec32(0x94000000u | ls(rx, ry, ext ? imm : (h & 0x1f) << 1)); return;  /* LHU */
    case 0x16: {                                                /* LWPC */
        if (ext && ms->slot) {
            reserved();
            return;
        }
        u32 v = mips_load(pcrel() + (ext ? imm : (h & 0xff) << 2), 4);
        if (!ms->exc)
            W(rx, v);
        return;
    }
    case 0x18: mips_exec32(0xa0000000u | ls(rx, ry, ext ? imm : h & 0x1f)); return;         /* SB */
    case 0x19: mips_exec32(0xa4000000u | ls(rx, ry, ext ? imm : (h & 0x1f) << 1)); return;  /* SH */
    case 0x1a:                                                  /* SW rx, imm(sp) */
        mips_exec32(0xac000000u | ls(29, rx, ext ? imm : (h & 0xff) << 2));
        return;
    case 0x1b: mips_exec32(0xac000000u | ls(rx, ry, ext ? imm : (h & 0x1f) << 2)); return;  /* SW */
    case 0x1c:                                                  /* RRR: ADDU, SUBU */
        if (ext)
            break;
        switch (h & 3) {
        case 1: W(gpr3[(h >> 2) & 7], R(rx) + R(ry)); return;
        case 3: W(gpr3[(h >> 2) & 7], R(rx) - R(ry)); return;
        }
        break;
    case 0x1d:                                                  /* RR */
        if (ext)
            break;
        switch (h & 0x1f) {
        case 0x00: {                                            /* JR, JRC, JALR, JALRC */
            int nd = (h >> 7) & 1, link = (h >> 6) & 1, ra = (h >> 5) & 1;
            jump(1, R(ra ? 31 : rx), 2, !nd, link ? 31 : 0);
            return;
        }
        case 0x01: mips_exec32(0x7000003fu); return;            /* SDBBP */
        case 0x02: W(24, (s32)R(rx) < (s32)R(ry)); return;      /* SLT */
        case 0x03: W(24, R(rx) < R(ry)); return;                /* SLTU */
        case 0x04: W(ry, R(ry) << (R(rx) & 31)); return;        /* SLLV */
        case 0x05: mips_raise(EXC_BP); return;                  /* BREAK */
        case 0x06: W(ry, R(ry) >> (R(rx) & 31)); return;        /* SRLV */
        case 0x07: W(ry, (u32)((s32)R(ry) >> (R(rx) & 31))); return;    /* SRAV */
        case 0x0a: W(24, R(rx) ^ R(ry)); return;                /* CMP */
        case 0x0b: W(rx, 0 - R(ry)); return;                    /* NEG */
        case 0x0c: W(rx, R(rx) & R(ry)); return;                /* AND */
        case 0x0d: W(rx, R(rx) | R(ry)); return;                /* OR */
        case 0x0e: W(rx, R(rx) ^ R(ry)); return;                /* XOR */
        case 0x0f: W(rx, ~R(ry)); return;                       /* NOT */
        case 0x10: W(rx, ms->hi[0]); return;                    /* MFHI */
        case 0x12: W(rx, ms->lo[0]); return;                    /* MFLO */
        case 0x11:                                              /* CNVT */
            switch ((h >> 5) & 7) {
            case 0: W(rx, R(rx) & 0xff); return;                /* ZEB */
            case 1: W(rx, R(rx) & 0xffff); return;              /* ZEH */
            case 4: W(rx, sext(R(rx), 8)); return;              /* SEB */
            case 5: W(rx, sext(R(rx), 16)); return;             /* SEH */
            }
            break;
        case 0x18: mips_exec32(0x00000018u | rx << 21 | ry << 16); return;  /* MULT */
        case 0x19: mips_exec32(0x00000019u | rx << 21 | ry << 16); return;  /* MULTU */
        case 0x1a: mips_exec32(0x0000001au | rx << 21 | ry << 16); return;  /* DIV */
        case 0x1b: mips_exec32(0x0000001bu | rx << 21 | ry << 16); return;  /* DIVU */
        }
        break;
    }
    reserved();
}

void mips16_exec(u32 i, int size)
{
    if (size == 2) {
        exec(i, 0, 0);
        return;
    }
    u32 h = i >> 16;
    if (((h >> 11) & 0x1f) == 0x1e) {                           /* EXTEND */
        exec(i & 0xffff, 1, h & 0x7ff);
        return;
    }
    /* JAL, JALX: a 26-bit index, its fields out of order */
    u32 idx = (h & 0x1f) << 21 | ((h >> 5) & 0x1f) << 16 | (i & 0xffff);
    u32 t = ((ms->pc + 4) & 0xf0000000u) | idx << 2;
    jump(1, (h & 0x400) ? t : t | 1, 4, 1, 31);
}
