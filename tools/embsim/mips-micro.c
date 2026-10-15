/* mips-micro.c -- microMIPS32 (Release 3, before Release 6), the
 * compressed instruction set of a PIC32MZ's microAptiv core (`pic32mz`),
 * which XC32's -mmicromips and clang's produce.
 *
 * An instruction is a 16-bit halfword, or two whose first's major
 * opcode (bits 15:10) is a 32-bit one's: those whose low three bits are
 * 1, 2 or 3 are 16-bit. A 32-bit instruction that does what a MIPS32 one
 * does is executed as that one (mips_exec32: its exceptions, its
 * accumulators, its coprocessor checks), with the operands moved to
 * where MIPS32 has them -- microMIPS puts rt in bits 25:21 and rs in
 * 20:16. What MIPS32 does not have is here: the 16-bit forms and their
 * three-bit register numbers, the branches and jumps (offsets in
 * halfwords; the 16-bit delay slots of jals, jalrs, jalrs16, bgezals and
 * bltzals; the compact beqzc, bnezc, jrc and jraddiusp that have none),
 * lwm, swm, lwp, swp, lwxs, addiupc and movep.
 *
 * The ISA mode: a jump's target carries it in bit 0, as on the part --
 * jr, jalr and their 16-bit forms to an even address go to MIPS32, jalx
 * switches, and a link register gets the mode it returns to in bit 0.
 * Exceptions save it in EPC's bit 0 (and the branch's address, for a
 * delay slot's), handlers run in Config3.ISAOnExc's, eret returns to
 * EPC's. As QEMU has it (the referee tests/golden/embsim-mips.sh holds
 * this file to): a branch in a delay slot, a delay slot of the size its
 * branch forbids, lwm, swm, lwp and swp in a delay slot, and lwp into
 * its own base are Reserved Instructions; a branch that can never be
 * taken (bne with one register twice, bltz and bgtz of $0) has no delay
 * slot.
 *
 * Not modelled: the FPU's microMIPS encodings (POOL32F, and bc1f/bc1t)
 * and the DSP ASE's, each ending the run as a lockup naming the
 * instruction. */
#include "mips.h"

static const u8 gpr3[8] = { 16, 17, 2, 3, 4, 5, 6, 7 };    /* a 3-bit register */
static const u8 gpr3s[8] = { 0, 17, 2, 3, 4, 5, 6, 7 };    /* a 16-bit store's */

static u32 R(u32 n) { return ms->r[n]; }
static void W(u32 n, u32 v) { if (n) ms->r[n] = v; }

static void reserved(void)
{
    mips_raise(EXC_RI);
}

static u32 sext(u32 v, int bits)
{
    return (u32)((s32)(v << (32 - bits)) >> (32 - bits));
}

int mips_micro_size(u32 first)
{
    u32 k = (first >> 10) & 7;
    return k >= 1 && k <= 3 ? 2 : 4;
}

int mips_micro_cost(u32 i, int size)
{
    u32 op = size == 2 ? (i >> 10) & 0x3f : i >> 26;
    switch (op) {
    case 0x02: case 0x0a: case 0x12: case 0x19: case 0x1a:     /* 16-bit loads */
    case 0x05: case 0x07: case 0x0d: case 0x0f: case 0x3f:     /* 32-bit loads */
        return 2;
    case 0x00:
        if (size == 4 && (i & 0x3f) == 0x3c) {                  /* POOL32AXF */
            u32 ext = (i >> 6) & 0x3f, minor = (i >> 12) & 0xf;
            if (ext == 0x2c && (minor == 0xa || minor == 0xb))  /* div, divu */
                return 18;
            if ((ext == 0x2c && minor >= 8) || ext == 0x2a || ext == 0x32)
                return 2;                                       /* the multiplies */
        }
        return 1;
    }
    return 1;
}

/* ---- branches ------------------------------------------------------------ */

/* A branch or jump at pc of `size` bytes: to `target` (the ISA bit in
 * bit 0) if taken, after its delay slot, which must be `ds` bytes (0:
 * either); `link`: the register that gets the address past the slot. A
 * compact one (ds -1) has no slot. */
static void jump(int taken, u32 target, int size, int ds, u32 link)
{
    u32 pc = ms->pc;
    if (ms->slot) {                 /* a branch in a delay slot */
        reserved();
        return;
    }
    if (link)
        W(link, pc + (u32)size + (u32)(ds > 0 ? ds : 4) + 1);
    if (ds < 0) {
        if (taken)
            ms->nx = target;
        return;
    }
    if (taken)
        ms->nnx = target;
    ms->nslot = 1;
    ms->nbpc = pc;
    ms->nds = ds;
    ms->nbtaken = taken;
}

/* whether the instruction at a does nothing: nop16 (move $0, $0) or the
 * 32-bit nop, ssnop, ehb */
static int is_nop(u32 a)
{
    u32 h, l;
    if (bus_debug_read(&ms->sim->bus, mips_phys(a), 2, &h))
        return 0;
    if (mips_micro_size(h) == 2)
        return h == 0x0c00;
    if (bus_debug_read(&ms->sim->bus, mips_phys(a + 2), 2, &l))
        return 0;
    h = h << 16 | l;
    return h == 0 || h == 0x0800 || h == 0x1800;
}

/* an unconditional relative branch: to itself, with nothing in its
 * slot, is an idle loop */
static void branch_always(u32 target, int size)
{
    if (target == (ms->pc | 1))
        mips_self_loop(target, is_nop(ms->pc + (u32)size));
    jump(1, target, size, 0, 0);
}

/* ---- the multiple loads and stores ------------------------------------- */

/* lwm and swm's register lists: s0 to s7 and fp, the first n of them,
 * then ra */
static const u8 mregs[9] = { 16, 17, 18, 19, 20, 21, 22, 23, 30 };

static void ldst_multiple(int store, u32 list, u32 va)
{
    u32 n = list & 15;
    if (ms->slot) {
        reserved();
        return;
    }
    if (n > 9)
        n = 0;
    for (u32 k = 0; k <= n && !ms->exc; k++) {
        u32 r = k < n ? mregs[k] : (list & 16) ? 31 : 0;
        if (k == n && !(list & 16))
            break;
        if (store) {
            mips_store(va, 4, R(r));
        } else {
            u32 v = mips_load(va, 4);
            if (!ms->exc)
                W(r, v);
        }
        va += 4;
    }
}

/* lwp and swp: rd and rd + 1 */
static void ldst_pair(int store, u32 rd, u32 base, u32 off)
{
    u32 va = R(base) + off;
    if (ms->slot || rd == 31 || (!store && rd == base)) {
        reserved();
        return;
    }
    if (store) {
        mips_store(va, 4, R(rd));
        mips_store(va + 4, 4, R(rd + 1));
        return;
    }
    u32 v = mips_load(va, 4);
    if (ms->exc)
        return;
    W(rd, v);
    v = mips_load(va + 4, 4);
    if (!ms->exc)
        W(rd + 1, v);
}

/* ---- the 16-bit instructions --------------------------------------------- */

/* MIPS32's encodings, for what is executed as one */
static u32 r3(u32 fn, u32 rs, u32 rt, u32 rd)
{
    return rs << 21 | rt << 16 | rd << 11 | fn;
}

static u32 i3(u32 op, u32 rs, u32 rt, u32 imm)
{
    return op << 26 | rs << 21 | rt << 16 | (imm & 0xffff);
}

static void pool16c(u32 h)
{
    u32 rd = gpr3[(h >> 3) & 7], rs = gpr3[h & 7], r5 = h & 31;
    switch ((h >> 4) & 0x3f) {
    case 0x00: case 0x01: case 0x02: case 0x03:                 /* NOT16 */
        W(rd, ~R(rs));
        return;
    case 0x04: case 0x05: case 0x06: case 0x07:                 /* XOR16 */
        W(rd, R(rd) ^ R(rs));
        return;
    case 0x08: case 0x09: case 0x0a: case 0x0b:                 /* AND16 */
        W(rd, R(rd) & R(rs));
        return;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:                 /* OR16 */
        W(rd, R(rd) | R(rs));
        return;
    case 0x10: case 0x11: case 0x12: case 0x13:                 /* LWM16 */
    case 0x14: case 0x15: case 0x16: case 0x17:                 /* SWM16 */
        ldst_multiple((h >> 6) & 1, 0x11 + ((h >> 4) & 3), R(29) + ((h & 15) << 2));
        return;
    case 0x18: case 0x19:                                       /* JR16 */
        jump(1, R(r5), 2, 0, 0);
        return;
    case 0x1a: case 0x1b:                                       /* JRC */
        jump(1, R(r5), 2, -1, 0);
        return;
    case 0x1c: case 0x1d:                                       /* JALR16 */
        jump(1, R(r5), 2, 4, 31);
        return;
    case 0x1e: case 0x1f:                                       /* JALRS16 */
        jump(1, R(r5), 2, 2, 31);
        return;
    case 0x20: case 0x21:                                       /* MFHI16 */
        W(r5, ms->hi[0]);
        return;
    case 0x24: case 0x25:                                       /* MFLO16 */
        W(r5, ms->lo[0]);
        return;
    case 0x28:                                                  /* BREAK16 */
        mips_raise(EXC_BP);
        return;
    case 0x2c:                                                  /* SDBBP16 */
        mips_exec32(0x7000003fu);
        return;
    case 0x30: case 0x31: {                                     /* JRADDIUSP */
        u32 to = R(31);
        jump(1, to, 2, -1, 0);
        if (!ms->exc)
            W(29, R(29) + ((h & 31) << 2));
        return;
    }
    }
    reserved();
}

static void exec16(u32 h)
{
    u32 rd = gpr3[(h >> 7) & 7], rs = gpr3[(h >> 4) & 7];
    u32 rs1 = gpr3[(h >> 1) & 7], rd5 = (h >> 5) & 31;
    switch ((h >> 10) & 0x3f) {
    case 0x01:                                                  /* POOL16A */
        W(rd, (h & 1) ? R(rs1) - R(rs) : R(rs1) + R(rs));       /* SUBU16, ADDU16 */
        return;
    case 0x09: {                                                /* POOL16B */
        u32 n = (h >> 1) & 7;
        if (!n)
            n = 8;
        W(rd, (h & 1) ? R(rs) >> n : R(rs) << n);               /* SRL16, SLL16 */
        return;
    }
    case 0x11: pool16c(h); return;                              /* POOL16C */
    case 0x19:                                                  /* LWGP */
        mips_exec32(i3(0x23, 28, rd, sext(h, 7) << 2));
        return;
    case 0x21: {                                                /* MOVEP */
        static const u8 dst[8][2] = { { 5, 6 }, { 5, 7 }, { 6, 7 }, { 4, 21 },
                                      { 4, 22 }, { 4, 5 }, { 4, 6 }, { 4, 7 } };
        static const u8 src[8] = { 0, 17, 2, 3, 16, 18, 19, 20 };
        if (h & 1) {
            reserved();
            return;
        }
        u32 a = R(src[(h >> 1) & 7]), b = R(src[(h >> 4) & 7]);
        W(dst[(h >> 7) & 7][0], a);
        W(dst[(h >> 7) & 7][1], b);
        return;
    }
    case 0x02: {                                                /* LBU16 */
        u32 off = h & 15;
        mips_exec32(i3(0x24, rs, rd, off == 15 ? 0xffffffffu : off));
        return;
    }
    case 0x0a: mips_exec32(i3(0x25, rs, rd, (h & 15) << 1)); return;   /* LHU16 */
    case 0x12: mips_exec32(i3(0x23, 29, rd5, (h & 31) << 2)); return;  /* LWSP */
    case 0x1a: mips_exec32(i3(0x23, rs, rd, (h & 15) << 2)); return;   /* LW16 */
    case 0x22:                                                  /* SB16 */
        mips_exec32(i3(0x28, rs, gpr3s[(h >> 7) & 7], h & 15));
        return;
    case 0x2a:                                                  /* SH16 */
        mips_exec32(i3(0x29, rs, gpr3s[(h >> 7) & 7], (h & 15) << 1));
        return;
    case 0x32: mips_exec32(i3(0x2b, 29, rd5, (h & 31) << 2)); return;  /* SWSP */
    case 0x3a:                                                  /* SW16 */
        mips_exec32(i3(0x2b, rs, gpr3s[(h >> 7) & 7], (h & 15) << 2));
        return;
    case 0x03: W(rd5, R(h & 31)); return;                       /* MOVE16 */
    case 0x0b: {                                                /* ANDI16 */
        static const u32 imm[16] = { 128, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32,
                                     63, 64, 255, 32768, 65535 };
        W(rd, R(rs) & imm[h & 15]);
        return;
    }
    case 0x13:                                                  /* POOL16D */
        if (h & 1) {                                            /* ADDIUSP */
            u32 e = (h >> 1) & 0x1ff;
            s32 d = e <= 1 ? 256 + (s32)e : e <= 255 ? (s32)e
                  : e <= 509 ? (s32)e - 512 : (s32)e - 768;
            W(29, R(29) + ((u32)d << 2));
        } else {                                                /* ADDIUS5 */
            W(rd5, R(rd5) + sext(h >> 1, 4));
        }
        return;
    case 0x1b:                                                  /* POOL16E */
        if (h & 1) {                                            /* ADDIUR1SP */
            W(rd, R(29) + (((h >> 1) & 0x3f) << 2));
        } else {                                                /* ADDIUR2 */
            static const int8_t imm[8] = { 1, 4, 8, 12, 16, 20, 24, -1 };
            W(rd, R(rs) + (u32)(s32)imm[(h >> 1) & 7]);
        }
        return;
    case 0x33:                                                  /* B16 */
        branch_always((ms->pc + 2 + (sext(h, 10) << 1)) | 1, 2);
        return;
    case 0x23: case 0x2b: {                                     /* BEQZ16, BNEZ16 */
        u32 t = (ms->pc + 2 + (sext(h, 7) << 1)) | 1;
        int z = R(rd) == 0;
        jump(((h >> 10) & 0x3f) == 0x23 ? z : !z, t, 2, 0, 0);
        return;
    }
    case 0x3b: {                                                /* LI16 */
        u32 v = h & 0x7f;
        W(rd, v == 0x7f ? 0xffffffffu : v);
        return;
    }
    }
    reserved();                     /* 0x29, 0x31, 0x39 */
}

/* ---- the 32-bit instructions --------------------------------------------- */

static void pool32axf(u32 i, u32 rt, u32 rs)
{
    u32 ext = (i >> 6) & 0x3f, minor = (i >> 12) & 0xf, ac = (i >> 14) & 3;
    switch (ext) {
    case 0x00: mips_exec32(r3(0x34, rs, rt, 0)); return;        /* TEQ */
    case 0x08: mips_exec32(r3(0x30, rs, rt, 0)); return;        /* TGE */
    case 0x10: mips_exec32(r3(0x31, rs, rt, 0)); return;        /* TGEU */
    case 0x20: mips_exec32(r3(0x32, rs, rt, 0)); return;        /* TLT */
    case 0x28: mips_exec32(r3(0x33, rs, rt, 0)); return;        /* TLTU */
    case 0x30: mips_exec32(r3(0x36, rs, rt, 0)); return;        /* TNE */
    case 0x03: case 0x23:                                       /* MFC0 */
        mips_exec32(0x40000000u | rt << 16 | rs << 11 | ((i >> 11) & 7));
        return;
    case 0x0b: case 0x2b:                                       /* MTC0 */
        mips_exec32(0x40800000u | rt << 16 | rs << 11 | ((i >> 11) & 7));
        return;
    case 0x2a: {                                                /* MADD, MADDU, MSUB, MSUBU $acN */
        static const u8 fn[4] = { 0x00, 0x01, 0x04, 0x05 };
        mips_exec32(0x70000000u | r3(fn[minor & 3], rs, rt, ac));
        return;
    }
    case 0x32:                                                  /* MULT, MULTU $acN */
        if ((minor & 3) > 1)
            break;
        mips_exec32(r3(0x18 + (minor & 1), rs, rt, ac));
        return;
    case 0x2c:
        switch (minor) {
        case 0x2: mips_exec32(0x7c000420u | rs << 16 | rt << 11); return;  /* SEB */
        case 0x3: mips_exec32(0x7c000620u | rs << 16 | rt << 11); return;  /* SEH */
        case 0x4: mips_exec32(0x70000021u | r3(0, rs, rt, rt)); return;    /* CLO */
        case 0x5: mips_exec32(0x70000020u | r3(0, rs, rt, rt)); return;    /* CLZ */
        case 0x6: mips_exec32(0x7c00003bu | rt << 16 | rs << 11); return;  /* RDHWR */
        case 0x7: mips_exec32(0x7c0000a0u | rs << 16 | rt << 11); return;  /* WSBH */
        case 0x8: mips_exec32(r3(0x18, rs, rt, 0)); return;                /* MULT */
        case 0x9: mips_exec32(r3(0x19, rs, rt, 0)); return;                /* MULTU */
        case 0xa: mips_exec32(r3(0x1a, rs, rt, 0)); return;                /* DIV */
        case 0xb: mips_exec32(r3(0x1b, rs, rt, 0)); return;                /* DIVU */
        case 0xc: mips_exec32(0x70000000u | r3(0x00, rs, rt, 0)); return;  /* MADD */
        case 0xd: mips_exec32(0x70000000u | r3(0x01, rs, rt, 0)); return;  /* MADDU */
        case 0xe: mips_exec32(0x70000000u | r3(0x04, rs, rt, 0)); return;  /* MSUB */
        case 0xf: mips_exec32(0x70000000u | r3(0x05, rs, rt, 0)); return;  /* MSUBU */
        }
        break;
    case 0x34:                                                  /* COP2's */
        if (minor == 4 || minor == 5 || minor == 8 || minor == 9 ||
            minor == 12 || minor == 13) {
            mips_raise_cpu(2);
            return;
        }
        break;
    case 0x3c:                                                  /* JALR(.HB), JALRS(.HB) */
        switch (minor) {
        case 0x0: case 0x1: jump(1, R(rs), 4, 4, rt); return;
        case 0x4: case 0x5: jump(1, R(rs), 4, 2, rt); return;
        }
        break;
    case 0x05:
        switch (minor) {
        case 0xe: mips_exec32(0x41400000u | rs << 16 | rt << 11); return;  /* RDPGPR */
        case 0xf: mips_exec32(0x41c00000u | rs << 16 | rt << 11); return;  /* WRPGPR */
        }
        break;
    case 0x0d:
        switch (minor) {
        case 0x0: mips_exec32(0x42000008u); return;             /* TLBP */
        case 0x1: mips_exec32(0x42000001u); return;             /* TLBR */
        case 0x2: mips_exec32(0x42000002u); return;             /* TLBWI */
        case 0x3: mips_exec32(0x42000006u); return;             /* TLBWR */
        case 0x9: mips_exec32(0x42000020u); return;             /* WAIT */
        case 0xe: mips_exec32(0x4200001fu); return;             /* DERET */
        case 0xf: mips_exec32(0x42000018u); return;             /* ERET */
        }
        break;
    case 0x1d:
        switch (minor) {
        case 0x4: mips_exec32(0x41606000u | rs << 16); return;  /* DI */
        case 0x5: mips_exec32(0x41606020u | rs << 16); return;  /* EI */
        }
        break;
    case 0x2d:
        switch (minor) {
        case 0x6: return;                                       /* SYNC */
        case 0x8: mips_raise(EXC_SYS); return;                  /* SYSCALL */
        case 0xd: mips_exec32(0x7000003fu); return;             /* SDBBP */
        }
        break;
    case 0x01: {                                                /* MFHI, MFLO, MTHI, MTLO $acN */
        static const u32 fn[4] = { 0x10, 0x12, 0x11, 0x13 };
        u32 f = fn[minor & 3];
        mips_exec32(f & 1 ? r3(f, rs, 0, ac) : r3(f, ac, 0, rs));
        return;
    }
    case 0x35: {                                                /* MFHI, MFLO, MTHI, MTLO */
        static const u32 fn[4] = { 0x10, 0x12, 0x11, 0x13 };
        if (minor > 3)
            break;
        u32 f = fn[minor];
        mips_exec32(f & 1 ? r3(f, rs, 0, 0) : r3(f, 0, 0, rs));
        return;
    }
    }
    reserved();
}

static void pool32a(u32 i, u32 rt, u32 rs, u32 rd)
{
    u32 sub = (i >> 6) & 0xf, rr = (i >> 6) & 31;
    switch (i & 0x3f) {
    case 0x00:                                                  /* the immediate shifts */
        switch (sub) {
        case 0x0: mips_exec32(r3(0x00, 0, rs, rt) | rd << 6); return;  /* SLL */
        case 0x1: mips_exec32(r3(0x02, 0, rs, rt) | rd << 6); return;  /* SRL */
        case 0x2: mips_exec32(r3(0x03, 0, rs, rt) | rd << 6); return;  /* SRA */
        case 0x3: mips_exec32(r3(0x02, 1, rs, rt) | rd << 6); return;  /* ROTR */
        }
        break;
    case 0x10: {                                                /* the register ALU */
        static const u8 fn[16] = { 0x04, 0x06, 0x07, 0x06, 0x20, 0x21, 0x22,
                                   0x23, 0, 0x24, 0x25, 0x27, 0x26, 0x2a, 0x2b, 0 };
        if (sub == 0x8) {                                       /* MUL */
            mips_exec32(0x70000002u | r3(0, rs, rt, rd));
            return;
        }
        if (sub == 0xf)
            break;
        mips_exec32(r3(fn[sub], rs, rt, rd) | (sub == 3 ? 1u << 6 : 0));   /* ROTRV */
        return;
    }
    case 0x18:
        switch (sub) {
        case 0x0: mips_exec32(r3(0x0b, rs, rt, rd)); return;    /* MOVN */
        case 0x1: mips_exec32(r3(0x0a, rs, rt, rd)); return;    /* MOVZ */
        case 0x4: {                                             /* LWXS rd, index(base) */
            u32 v = mips_load(R(rs) + (R(rt) << 2), 4);
            if (!ms->exc)
                W(rd, v);
            return;
        }
        }
        break;
    case 0x0c:                                                  /* INS */
        mips_exec32(0x7c000004u | rs << 21 | rt << 16 | rd << 11 | rr << 6);
        return;
    case 0x2c:                                                  /* EXT */
        mips_exec32(0x7c000000u | rs << 21 | rt << 16 | rd << 11 | rr << 6);
        return;
    case 0x3c:
        pool32axf(i, rt, rs);
        return;
    case 0x07:                                                  /* BREAK */
        mips_raise(EXC_BP);
        return;
    }
    reserved();
}

static void pool32i(u32 i, u32 rs)
{
    u32 t = (ms->pc + 4 + (sext(i, 16) << 1)) | 1;
    s32 a = (s32)R(rs);
    switch ((i >> 21) & 31) {
    case 0x00: if (rs) jump(a < 0, t, 4, 0, 0); return;         /* BLTZ */
    case 0x01: jump(a < 0, t, 4, 4, 31); return;                /* BLTZAL */
    case 0x11: jump(a < 0, t, 4, 2, 31); return;                /* BLTZALS */
    case 0x02:                                                  /* BGEZ */
        if (!rs)
            branch_always(t, 4);
        else
            jump(a >= 0, t, 4, 0, 0);
        return;
    case 0x03: jump(a >= 0, t, 4, 4, 31); return;               /* BGEZAL */
    case 0x13: jump(a >= 0, t, 4, 2, 31); return;               /* BGEZALS */
    case 0x04: jump(a <= 0, t, 4, 0, 0); return;                /* BLEZ */
    case 0x06: if (rs) jump(a > 0, t, 4, 0, 0); return;         /* BGTZ */
    case 0x05: jump(a != 0, t, 4, -1, 0); return;               /* BNEZC */
    case 0x07: jump(a == 0, t, 4, -1, 0); return;               /* BEQZC */
    case 0x08: mips_exec32(i3(1, rs, 0x0a, i)); return;         /* TLTI */
    case 0x09: mips_exec32(i3(1, rs, 0x08, i)); return;         /* TGEI */
    case 0x0a: mips_exec32(i3(1, rs, 0x0b, i)); return;         /* TLTIU */
    case 0x0b: mips_exec32(i3(1, rs, 0x09, i)); return;         /* TGEIU */
    case 0x0c: mips_exec32(i3(1, rs, 0x0e, i)); return;         /* TNEI */
    case 0x0e: mips_exec32(i3(1, rs, 0x0c, i)); return;         /* TEQI */
    case 0x0d: W(rs, i << 16); return;                          /* LUI */
    case 0x10: return;                                          /* SYNCI */
    case 0x14: case 0x15: mips_raise_cpu(2); return;            /* BC2F, BC2T */
    case 0x1c: case 0x1d:                                       /* BC1F, BC1T */
        mips_unmodelled("a microMIPS FPU instruction");
        return;
    }
    reserved();
}

static void exec32(u32 i)
{
    u32 op = i >> 26, rt = (i >> 21) & 31, rs = (i >> 16) & 31;
    u32 rd = (i >> 11) & 31, pc = ms->pc;
    switch (op) {
    case 0x00: pool32a(i, rt, rs, rd); return;
    case 0x08: {                                                /* POOL32B */
        u32 off = sext(i, 12);
        switch ((i >> 12) & 0xf) {
        case 0x0: case 0x8: mips_raise_cpu(2); return;          /* LWC2, SWC2 */
        case 0x1: ldst_pair(0, rt, rs, off); return;            /* LWP */
        case 0x9: ldst_pair(1, rt, rs, off); return;            /* SWP */
        case 0x5: ldst_multiple(0, rt, R(rs) + off); return;    /* LWM32 */
        case 0xd: ldst_multiple(1, rt, R(rs) + off); return;    /* SWM32 */
        case 0x6: mips_exec32(i3(0x2f, rs, rt, off)); return;   /* CACHE */
        }
        break;
    }
    case 0x18: {                                                /* POOL32C */
        u32 off = sext(i, 12);
        switch ((i >> 12) & 0xf) {
        case 0x0: mips_exec32(i3(0x22, rs, rt, off)); return;   /* LWL */
        case 0x1: mips_exec32(i3(0x26, rs, rt, off)); return;   /* LWR */
        case 0x8: mips_exec32(i3(0x2a, rs, rt, off)); return;   /* SWL */
        case 0x9: mips_exec32(i3(0x2e, rs, rt, off)); return;   /* SWR */
        case 0x2: return;                                       /* PREF */
        case 0x3: mips_exec32(i3(0x30, rs, rt, off)); return;   /* LL */
        case 0xb: mips_exec32(i3(0x38, rs, rt, off)); return;   /* SC */
        }
        break;
    }
    case 0x10: pool32i(i, rs); return;
    case 0x04: mips_exec32(i3(0x08, rs, rt, i)); return;        /* ADDI32 */
    case 0x0c: mips_exec32(i3(0x09, rs, rt, i)); return;        /* ADDIU32 */
    case 0x14: mips_exec32(i3(0x0d, rs, rt, i)); return;        /* ORI32 */
    case 0x1c: mips_exec32(i3(0x0e, rs, rt, i)); return;        /* XORI32 */
    case 0x34: mips_exec32(i3(0x0c, rs, rt, i)); return;        /* ANDI32 */
    case 0x24: mips_exec32(i3(0x0a, rs, rt, i)); return;        /* SLTI32 */
    case 0x2c: mips_exec32(i3(0x0b, rs, rt, i)); return;        /* SLTIU32 */
    case 0x07: mips_exec32(i3(0x20, rs, rt, i)); return;        /* LB32 */
    case 0x05: mips_exec32(i3(0x24, rs, rt, i)); return;        /* LBU32 */
    case 0x0f: mips_exec32(i3(0x21, rs, rt, i)); return;        /* LH32 */
    case 0x0d: mips_exec32(i3(0x25, rs, rt, i)); return;        /* LHU32 */
    case 0x3f: mips_exec32(i3(0x23, rs, rt, i)); return;        /* LW32 */
    case 0x06: mips_exec32(i3(0x28, rs, rt, i)); return;        /* SB32 */
    case 0x0e: mips_exec32(i3(0x29, rs, rt, i)); return;        /* SH32 */
    case 0x3e: mips_exec32(i3(0x2b, rs, rt, i)); return;        /* SW32 */
    case 0x27: mips_exec32(i3(0x31, rs, rt, i)); return;        /* LWC1 */
    case 0x2f: mips_exec32(i3(0x35, rs, rt, i)); return;        /* LDC1 */
    case 0x26: mips_exec32(i3(0x39, rs, rt, i)); return;        /* SWC1 */
    case 0x2e: mips_exec32(i3(0x3d, rs, rt, i)); return;        /* SDC1 */
    case 0x25: case 0x2d: {                                     /* BEQ32, BNE32 */
        u32 t = (pc + 4 + (sext(i, 16) << 1)) | 1;
        int eq = R(rt) == R(rs);
        if (rt == rs) {
            if (op == 0x25)
                branch_always(t, 4);
        } else {
            jump(op == 0x25 ? eq : !eq, t, 4, 0, 0);
        }
        return;
    }
    case 0x35: {                                                /* J32 */
        u32 t = ((pc + 4) & 0xf8000000u) | (i & 0x03ffffffu) << 1 | 1;
        branch_always(t, 4);
        return;
    }
    case 0x3d:                                                  /* JAL32 */
        jump(1, ((pc + 4) & 0xf8000000u) | (i & 0x03ffffffu) << 1 | 1, 4, 4, 31);
        return;
    case 0x1d:                                                  /* JALS32 */
        jump(1, ((pc + 4) & 0xf8000000u) | (i & 0x03ffffffu) << 1 | 1, 4, 2, 31);
        return;
    case 0x3c:                                                  /* JALX32: to MIPS32 */
        jump(1, ((pc + 4) & 0xf0000000u) | (i & 0x03ffffffu) << 2, 4, 4, 31);
        return;
    case 0x1e: {                                                /* ADDIUPC */
        u32 base = ms->slot ? ms->bpc & ~1u : pc;
        W(gpr3[(i >> 23) & 7], (base & ~3u) + (sext(i, 23) << 2));
        return;
    }
    case 0x15:                                                  /* POOL32F */
        if (!ms->fpu) {
            reserved();
            return;
        }
        if (!(ms->status & ST_CU1)) {
            mips_raise_cpu(1);
            return;
        }
        mips_unmodelled("a microMIPS FPU instruction");
        return;
    }
    reserved();
}

void mips_micro_exec(u32 i, int size)
{
    if (size == 2)
        exec16(i);
    else
        exec32(i);
}
