/* riscv.c -- the RISC-V core: RV32 and RV64 with the I, M, A, F, D and
 * C extensions, Zicsr and Zifencei, in machine mode (and user mode, for
 * an mret to it).
 *
 * The core starts at QEMU's reset vector, 0x1000, where the virt board's
 * reset ROM (virt-rom.c) jumps to the image as QEMU's does, so the two
 * run the same instructions from the first. Traps are the privileged
 * architecture's: mtvec (direct or vectored), mepc, mcause, mtval and
 * mstatus's MIE, MPIE and MPP; the interrupts are the CLINT's software
 * and timer interrupts (clint.c), through mip and mie, and WFI waits for
 * them. The counters are the run's: mcycle is the estimated cycles
 * (tools/bench/cost.h's RV32 table), minstret the instructions, and the
 * CLINT's mtime counts with mcycle.
 *
 * Memory is the bus at 32 bits: an RV64 address above 4 GiB is an access
 * fault. Ordinary loads and stores may be misaligned, as on QEMU; LR, SC
 * and the AMOs may not. SC succeeds when the reservation's address and
 * the value LR read are still there, which is QEMU's rule.
 *
 * A compressed instruction is expanded into the 32-bit instruction it
 * stands for, and that is executed, so the two cannot disagree; its link
 * address is the next instruction's, two bytes on. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "../bench/cost.h"
#include "devices.h"
#include "riscv.h"

struct rv_state *rs;

/* ---- traps -------------------------------------------------------------- */

void rv_trap(u64 cause, u64 tval)
{
    if (rs->trap)
        return;
    rs->trap = 1;
    rs->cause = cause;
    rs->tval = tval;
}

void rv_illegal(void)
{
    rv_trap(EXC_ILLEGAL, rs->insn);
}

/* trap entry: an exception (cause), or an interrupt (intr), taken at pc */
static void trap_enter(u64 cause, u64 tval, int intr)
{
    u64 st = rs->mstatus;
    rs->mepc = rs->pc;
    rs->mcause = intr ? (1ull << (rs->xlen - 1)) | cause : cause;
    rs->mtval = rv_xl(tval);
    st = (st & ~(u64)MSTATUS_MPIE) | ((st & MSTATUS_MIE) ? MSTATUS_MPIE : 0);
    st &= ~(u64)MSTATUS_MIE;
    st = (st & ~(u64)MSTATUS_MPP) | (u64)rs->priv << 11;
    rs->mstatus = st;
    rs->priv = PRV_M;
    rs->resv = 0;
    u64 base = rs->mtvec & ~(u64)3;
    rs->pc = intr && (rs->mtvec & 1) ? base + 4 * cause : base;
    if (rs->sim->an)
        an_exc_entry(rs->sim, (u32)cause | (intr ? 0x80000000u : 0),
                     (u32)rs->mepc);
}

/* ---- memory ------------------------------------------------------------- */

/* A load or store of the core's. A debugger's watchpoint stops the
 * instruction before the access, as QEMU's stub does (gdb then steps it
 * with the watchpoint out). */
u64 rv_load(u64 a, int n)
{
    struct bus *b = &rs->sim->bus;
    if (rs->trap)
        return 0;
    if ((a >> 32) || ((a + (u64)n - 1) >> 32)) {
        rv_trap(EXC_LOAD_ACCESS, a);
        return 0;
    }
    u32 a32 = (u32)a;
    if (b->nwatch && bus_watch_check(b, a32, n, 0)) {
        rv_trap(TRAP_WATCH, 0);
        return 0;
    }
    struct region *r = bus_region(b, a32, (u32)n);
    u64 v = 0;
    if (r) {
        const u8 *p = r->mem + (a32 - r->base);
        for (int i = n - 1; i >= 0; i--)
            v = v << 8 | p[i];
        return v;
    }
    for (int i = 0; i < n;) {
        u32 x;
        int k = n - i >= 4 && !((a32 + (u32)i) & 3) ? 4
              : n - i >= 2 && !((a32 + (u32)i) & 1) ? 2 : 1;
        if (bus_read(b, a32 + (u32)i, k, &x)) {
            rv_trap(EXC_LOAD_ACCESS, a);
            return 0;
        }
        v |= (u64)x << (8 * i);
        i += k;
    }
    return v;
}

void rv_store(u64 a, int n, u64 v)
{
    struct bus *b = &rs->sim->bus;
    if (rs->trap)
        return;
    if ((a >> 32) || ((a + (u64)n - 1) >> 32)) {
        rv_trap(EXC_STORE_ACCESS, a);
        return;
    }
    u32 a32 = (u32)a;
    if (b->nwatch && bus_watch_check(b, a32, n, 1)) {
        rv_trap(TRAP_WATCH, 0);
        return;
    }
    struct region *r = bus_region(b, a32, (u32)n);
    if (r) {
        if (!r->rom) {
            u8 *p = r->mem + (a32 - r->base);
            for (int i = 0; i < n; i++)
                p[i] = (u8)(v >> (8 * i));
        }
        return;
    }
    for (int i = 0; i < n;) {
        int k = n - i >= 4 && !((a32 + (u32)i) & 3) ? 4
              : n - i >= 2 && !((a32 + (u32)i) & 1) ? 2 : 1;
        u32 x = (u32)(v >> (8 * i));
        if (k < 4)
            x &= (1u << (8 * k)) - 1;
        if (bus_write(b, a32 + (u32)i, k, x)) {
            rv_trap(EXC_STORE_ACCESS, a);
            return;
        }
        i += k;
    }
}

/* ---- the counters and the CSRs ---------------------------------------- */

u64 rv_mtime(void)
{
    return rs->sim->cycles + rs->mtime_off;
}

u64 rv_mip(void)
{
    u64 p = 0;
    if (rs->msip & 1)
        p |= MIP_MSIP;
    if (rv_mtime() >= rs->mtimecmp)
        p |= MIP_MTIP;
    return p;
}

static u64 mstatus_rd(void)
{
    u64 v = rs->mstatus;
    if ((v & MSTATUS_FS) == MSTATUS_FS)
        v |= 1ull << (rs->xlen - 1);            /* SD */
    if (rs->xlen == 64)
        v |= 0xa00000000ull;                    /* UXL = SXL = 64 */
    return rv_xl(v);
}

static u64 misa(void)
{
    /* A C D F I M U, and the base's width */
    u64 ext = 1u << 0 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 8 | 1u << 12 |
              1u << 20;
    return rs->xlen == 64 ? 2ull << 62 | ext : 1ull << 30 | ext;
}

static int fs_on(void)
{
    return (rs->mstatus & MSTATUS_FS) != 0;
}

void rv_fp_dirty(void)
{
    rs->mstatus |= MSTATUS_FS;
}

/* A CSR's value: 0 when there is no such CSR. A debugger's read (debug)
 * passes the floating-point unit's being off. */
static int csr_rd(u32 csr, u64 *v, int debug)
{
    int x32 = rs->xlen == 32;
    u64 cyc = rs->cyc_now + rs->mcycle_off;
    u64 ins = rs->insn_now + rs->minstret_off;
    if (csr >= 1 && csr <= 3) {
        if (!fs_on() && !debug)
            return 0;
        *v = csr == 1 ? rs->fflags : csr == 2 ? rs->frm
                                  : rs->frm << 5 | rs->fflags;
        return 1;
    }
    if (csr >= 0x323 && csr <= 0x33f) {         /* mhpmevent3..31 */
        *v = 0;
        return 1;
    }
    if (csr >= 0x3a0 && csr <= 0x3af) {         /* pmpcfg */
        if (!x32 && (csr & 1))
            return 0;
        *v = rv_xl(rs->pmpcfg[csr - 0x3a0]);
        return 1;
    }
    if (csr >= 0x3b0 && csr <= 0x3ef) {         /* pmpaddr */
        *v = rv_xl(rs->pmpaddr[csr - 0x3b0]);
        return 1;
    }
    if ((csr >= 0xb03 && csr <= 0xb1f) || (csr >= 0xc03 && csr <= 0xc1f) ||
        (x32 && ((csr >= 0xb83 && csr <= 0xb9f) ||
                 (csr >= 0xc83 && csr <= 0xc9f)))) {
        *v = 0;                                 /* hpmcounters */
        return 1;
    }
    switch (csr) {
    case 0x300: *v = mstatus_rd(); return 1;
    case 0x301: *v = misa(); return 1;
    case 0x302: *v = rs->medeleg; return 1;
    case 0x303: *v = rs->mideleg; return 1;
    case 0x304: *v = rs->mie; return 1;
    case 0x305: *v = rs->mtvec; return 1;
    case 0x306: *v = rs->mcounteren; return 1;
    case 0x30a: *v = rv_xl(rs->menvcfg); return 1;
    case 0x310: if (!x32) return 0; *v = 0; return 1;           /* mstatush */
    case 0x31a: if (!x32) return 0; *v = rs->menvcfg >> 32; return 1;
    case 0x320: *v = rs->mcountinhibit; return 1;
    case 0x340: *v = rs->mscratch; return 1;
    case 0x341: *v = rs->mepc; return 1;
    case 0x342: *v = rs->mcause; return 1;
    case 0x343: *v = rs->mtval; return 1;
    case 0x344: *v = rv_mip(); return 1;
    case 0xb00: case 0xc00: *v = rv_xl(cyc); return 1;
    case 0xb02: case 0xc02: *v = rv_xl(ins); return 1;
    case 0xc01: *v = rv_xl(rv_mtime()); return 1;
    case 0xb80: case 0xc80: if (!x32) return 0; *v = cyc >> 32; return 1;
    case 0xb82: case 0xc82: if (!x32) return 0; *v = ins >> 32; return 1;
    case 0xc81: if (!x32) return 0; *v = rv_mtime() >> 32; return 1;
    case 0xf11: case 0xf12: case 0xf13: case 0xf14: case 0xf15:
        *v = 0;                                 /* the IDs, the hart: 0 */
        return 1;
    }
    return 0;
}

static u64 set_lo(u64 old, u64 v) { return (old & ~0xffffffffull) | (u32)v; }
static u64 set_hi(u64 old, u64 v) { return (old & 0xffffffffull) | v << 32; }

/* a write to a CSR that exists and may be written */
static void csr_wr(u32 csr, u64 v)
{
    int x32 = rs->xlen == 32;
    u64 cyc = rs->cyc_now + rs->mcycle_off;
    u64 ins = rs->insn_now + rs->minstret_off;
    switch (csr) {
    case 1: rs->fflags = (u32)v & 31; rv_fp_dirty(); return;
    case 2: rs->frm = (u32)v & 7; rv_fp_dirty(); return;
    case 3:
        rs->fflags = (u32)v & 31;
        rs->frm = (u32)(v >> 5) & 7;
        rv_fp_dirty();
        return;
    case 0x300: {
        u64 mask = MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_FS | MSTATUS_MPRV |
                   MSTATUS_TW;
        u64 mpp = (v >> 11) & 3;
        if (mpp == PRV_U || mpp == PRV_M)
            mask |= MSTATUS_MPP;
        rs->mstatus = (rs->mstatus & ~mask) | (v & mask);
        return;
    }
    case 0x304: rs->mie = v & (MIP_MSIP | MIP_MTIP | 1u << 11); return;
    case 0x305:
        if ((v & 3) < 2)
            rs->mtvec = rv_xl(v);
        return;
    case 0x306: rs->mcounteren = (u32)v; return;
    case 0x30a: rs->menvcfg = x32 ? set_lo(rs->menvcfg, v) : v; return;
    case 0x31a: rs->menvcfg = set_hi(rs->menvcfg, v); return;
    case 0x320: rs->mcountinhibit = (u32)v & ~2u; return;
    case 0x340: rs->mscratch = rv_xl(v); return;
    case 0x341: rs->mepc = rv_xl(v) & ~(u64)1; return;
    case 0x342: rs->mcause = rv_xl(v); return;
    case 0x343: rs->mtval = rv_xl(v); return;
    /* mcycle: the next instruction sees the value written plus this one's
     * cycles; minstret: exactly the value written (this one is not
     * counted, the write is in place of the count) */
    case 0xb00:
        rs->mcycle_off += (x32 ? set_lo(cyc, v) : v) - cyc;
        return;
    case 0xb80:
        rs->mcycle_off += set_hi(cyc, v) - cyc;
        return;
    case 0xb02:
        rs->minstret_off += (x32 ? set_lo(ins + 1, v) : v) - (ins + 1);
        return;
    case 0xb82:
        rs->minstret_off += set_hi(ins + 1, v) - (ins + 1);
        return;
    }
    if (csr >= 0x3a0 && csr <= 0x3af)
        rs->pmpcfg[csr - 0x3a0] = rv_xl(v);
    else if (csr >= 0x3b0 && csr <= 0x3ef)
        rs->pmpaddr[csr - 0x3b0] = rv_xl(v) & (x32 ? 0xffffffffull
                                                   : 0x3fffffffffffffull);
    /* the rest (misa, medeleg, mideleg, mip, the event counters) keep
     * their values */
}

/* CSRRW, CSRRS, CSRRC and their immediate forms */
static void csr_insn(u32 i)
{
    u32 csr = i >> 20, rd = (i >> 7) & 31, rs1 = (i >> 15) & 31;
    u32 f3 = (i >> 12) & 7;
    u64 src = (f3 & 4) ? rs1 : rs->x[rs1];
    int write = (f3 & 3) == 1 || rs1 != 0;
    u64 old;
    if (((csr >> 8) & 3) > (u32)rs->priv ||
        (write && (csr >> 10) == 3) || !csr_rd(csr, &old, 0)) {
        rv_illegal();
        return;
    }
    if (rs->priv < PRV_M && csr >= 0xc00 && csr <= 0xc1f &&
        !((rs->mcounteren >> (csr & 31)) & 1)) {
        rv_illegal();
        return;
    }
    if (write) {
        u64 nv = (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? old | src
                                                     : old & ~src;
        csr_wr(csr, nv);
    }
    rv_wx(rd, old);
}

/* ---- the integer instructions ------------------------------------------ */

static void jump(u64 t)
{
    rs->npc = rv_xl(t);
}

static u64 mulhu64(u64 a, u64 b)
{
    u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (u32)lh + (u32)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

/* MULH, MULHSU, MULHU (f3 1, 2, 3) at the core's width */
static u64 mulh(u32 f3, u64 a, u64 b)
{
    if (rs->xlen == 32) {
        s64 sa = (s32)(u32)a, sb = (s32)(u32)b;
        u64 ua = (u32)a, ub = (u32)b;
        if (f3 == 1)
            return (u64)((sa * sb) >> 32);
        if (f3 == 2)
            return (u64)((sa * (s64)ub) >> 32);
        return (ua * ub) >> 32;
    }
    u64 h = mulhu64(a, b);
    if (f3 == 1 || f3 == 2)
        if ((s64)a < 0)
            h -= b;
    if (f3 == 1 && (s64)b < 0)
        h -= a;
    return h;
}

/* DIV, DIVU, REM, REMU (f3 4..7) on `w`-bit values (32 or 64), with the
 * architecture's results for a zero divisor and for overflow */
static u64 divrem(u32 f3, u64 a, u64 b, int w)
{
    u64 m = w == 32 ? 0xffffffffull : ~0ull;
    s64 sa = w == 32 ? (s32)(u32)a : (s64)a;
    s64 sb = w == 32 ? (s32)(u32)b : (s64)b;
    u64 ua = a & m, ub = b & m;
    s64 smin = w == 32 ? (s64)INT32_MIN : INT64_MIN;
    switch (f3) {
    case 4:
        if (!sb)
            return ~0ull;
        if (sa == smin && sb == -1)
            return (u64)sa;
        return (u64)(sa / sb);
    case 5:
        return ub ? ua / ub : ~0ull;
    case 6:
        if (!sb)
            return (u64)sa;
        if (sa == smin && sb == -1)
            return 0;
        return (u64)(sa % sb);
    default:
        return ub ? ua % ub : (u64)sa;
    }
}

static u64 sext32(u64 v)
{
    return (u64)(s64)(s32)(u32)v;
}

static void op_imm(u32 i, u32 rd, u32 f3, u64 a, s64 imm)
{
    int sh = rs->xlen == 64 ? 63 : 31;
    u32 shamt = (i >> 20) & (u32)sh;
    u32 top = rs->xlen == 64 ? i >> 26 : i >> 25;
    switch (f3) {
    case 0: rv_wx(rd, a + (u64)imm); return;
    case 2: rv_wx(rd, rv_sx(a) < imm); return;
    case 3: rv_wx(rd, rv_xl(a) < rv_xl((u64)imm)); return;
    case 4: rv_wx(rd, a ^ (u64)imm); return;
    case 6: rv_wx(rd, a | (u64)imm); return;
    case 7: rv_wx(rd, a & (u64)imm); return;
    case 1:
        if (top != 0)
            break;
        rv_wx(rd, a << shamt);
        return;
    case 5:
        if ((top & ~(rs->xlen == 64 ? 0x10u : 0x20u)) != 0)
            break;
        if (i & (1u << 30))
            rv_wx(rd, (u64)(rv_sx(a) >> shamt));
        else
            rv_wx(rd, rv_xl(a) >> shamt);
        return;
    }
    rv_illegal();
}

static void op_reg(u32 rd, u32 f3, u32 f7, u64 a, u64 b)
{
    int sh = rs->xlen == 64 ? 63 : 31;
    if (f7 == 1) {
        if (f3 == 0)
            rv_wx(rd, a * b);
        else if (f3 < 4)
            rv_wx(rd, mulh(f3, a, b));
        else
            rv_wx(rd, divrem(f3, a, b, rs->xlen));
        return;
    }
    if (f7 == 0x20 && f3 != 0 && f3 != 5) {
        rv_illegal();
        return;
    }
    if (f7 != 0 && f7 != 0x20) {
        rv_illegal();
        return;
    }
    switch (f3) {
    case 0: rv_wx(rd, f7 ? a - b : a + b); return;
    case 1: rv_wx(rd, a << (b & (u64)sh)); return;
    case 2: rv_wx(rd, rv_sx(a) < rv_sx(b)); return;
    case 3: rv_wx(rd, a < b); return;
    case 4: rv_wx(rd, a ^ b); return;
    case 5:
        if (f7)
            rv_wx(rd, (u64)(rv_sx(a) >> (b & (u64)sh)));
        else
            rv_wx(rd, a >> (b & (u64)sh));
        return;
    case 6: rv_wx(rd, a | b); return;
    default: rv_wx(rd, a & b); return;
    }
}

/* the RV64 word operations: OP-IMM-32 and OP-32 */
static void op_w(u32 i, u32 rd, u32 f3, u32 f7, u64 a, u64 b, int imm_form)
{
    if (rs->xlen != 64) {
        rv_illegal();
        return;
    }
    if (imm_form) {
        u32 shamt = (i >> 20) & 31;
        s64 imm = (s32)i >> 20;
        if (f3 == 0)
            rv_wx(rd, sext32(a + (u64)imm));
        else if (f3 == 1 && f7 == 0)
            rv_wx(rd, sext32((u32)a << shamt));
        else if (f3 == 5 && f7 == 0)
            rv_wx(rd, sext32((u32)a >> shamt));
        else if (f3 == 5 && f7 == 0x20)
            rv_wx(rd, (u64)(s64)((s32)(u32)a >> shamt));
        else
            rv_illegal();
        return;
    }
    u32 sh = (u32)b & 31;
    if (f7 == 1) {
        if (f3 == 0)
            rv_wx(rd, sext32((u32)a * (u32)b));
        else if (f3 >= 4)
            rv_wx(rd, sext32(divrem(f3, a, b, 32)));
        else
            rv_illegal();
        return;
    }
    if (f3 == 0 && f7 == 0)
        rv_wx(rd, sext32(a + b));
    else if (f3 == 0 && f7 == 0x20)
        rv_wx(rd, sext32(a - b));
    else if (f3 == 1 && f7 == 0)
        rv_wx(rd, sext32((u32)a << sh));
    else if (f3 == 5 && f7 == 0)
        rv_wx(rd, sext32((u32)a >> sh));
    else if (f3 == 5 && f7 == 0x20)
        rv_wx(rd, (u64)(s64)((s32)(u32)a >> sh));
    else
        rv_illegal();
}

/* LR, SC and the AMOs, .W and .D */
static void amo(u32 i, u32 rd, u32 f3, u64 a, u64 b)
{
    int n = f3 == 2 ? 4 : f3 == 3 && rs->xlen == 64 ? 8 : 0;
    u32 f5 = i >> 27;
    if (!n) {
        rv_illegal();
        return;
    }
    u64 m = n == 4 ? 0xffffffffull : ~0ull;
    if (f5 == 2) {                              /* LR */
        if ((i >> 20) & 31) {
            rv_illegal();
            return;
        }
        if (a & (u64)(n - 1)) {
            rv_trap(EXC_LOAD_ALIGN, a);
            return;
        }
        u64 v = rv_load(a, n);
        if (rs->trap)
            return;
        rs->resv = 1;
        rs->resv_addr = a;
        rs->resv_val = v;
        rv_wx(rd, n == 4 ? sext32(v) : v);
        return;
    }
    if (a & (u64)(n - 1)) {
        rv_trap(EXC_STORE_ALIGN, a);
        return;
    }
    if (f5 == 3) {                              /* SC */
        int ok = rs->resv && rs->resv_addr == a;
        if (ok) {
            u64 cur = rv_load(a, n);
            if (rs->trap) {
                if (rs->cause == EXC_LOAD_ACCESS)
                    rs->cause = EXC_STORE_ACCESS;
                return;
            }
            ok = cur == rs->resv_val;
        }
        if (ok)
            rv_store(a, n, b & m);
        if (rs->trap)
            return;
        rs->resv = 0;
        rv_wx(rd, !ok);
        return;
    }
    u64 old = rv_load(a, n);
    if (rs->trap) {
        if (rs->cause == EXC_LOAD_ACCESS)
            rs->cause = EXC_STORE_ACCESS;       /* an AMO's fault */
        return;
    }
    s64 so = n == 4 ? (s32)(u32)old : (s64)old;
    s64 sb = n == 4 ? (s32)(u32)b : (s64)b;
    u64 uo = old & m, ub = b & m, nv;
    switch (f5) {
    case 0x01: nv = b; break;
    case 0x00: nv = old + b; break;
    case 0x04: nv = old ^ b; break;
    case 0x0c: nv = old & b; break;
    case 0x08: nv = old | b; break;
    case 0x10: nv = so < sb ? old : b; break;
    case 0x14: nv = so > sb ? old : b; break;
    case 0x18: nv = uo < ub ? old : b; break;
    case 0x1c: nv = uo > ub ? old : b; break;
    default: rv_illegal(); return;
    }
    rv_store(a, n, nv & m);
    if (rs->trap)
        return;
    rv_wx(rd, n == 4 ? sext32(old) : old);
}

static void wfi(void)
{
    struct sim *s = rs->sim;
    u32 left;
    if (rs->priv < PRV_M && (rs->mstatus & MSTATUS_TW)) {
        rv_illegal();
        return;
    }
    if (rs->mie & rv_mip())
        return;
    if (rs->mie && sim_next_event(s, &left)) {
        s->cycles += left;
        sim_advance(s, left);
        return;
    }
    sim_end(s, END_IDLE,
            "waiting for an interrupt that nothing can raise, at 0x%08llx",
            (unsigned long long)rs->pc);
}

static void sys_insn(u32 i, u32 rd, u32 f3)
{
    if (f3 != 0) {
        if (f3 == 4)
            rv_illegal();
        else
            csr_insn(i);
        return;
    }
    switch (i) {
    case 0x00000073:                            /* ECALL */
        rv_trap(rs->priv == PRV_M ? EXC_ECALL_M : EXC_ECALL_U, 0);
        return;
    case 0x00100073:                            /* EBREAK */
        rv_trap(EXC_BREAKPOINT, rs->pc);
        return;
    case 0x30200073: {                          /* MRET */
        if (rs->priv != PRV_M) {
            rv_illegal();
            return;
        }
        u64 st = rs->mstatus;
        int mpp = (int)((st >> 11) & 3);
        st = (st & ~(u64)MSTATUS_MIE) | ((st & MSTATUS_MPIE) ? MSTATUS_MIE : 0);
        st |= MSTATUS_MPIE;
        st &= ~(u64)MSTATUS_MPP;                /* to U, the least privileged */
        if (mpp != PRV_M)
            st &= ~(u64)MSTATUS_MPRV;
        rs->mstatus = st;
        rs->priv = mpp;
        jump(rs->mepc);
        if (rs->sim->an)
            an_exc_return(rs->sim);
        return;
    }
    case 0x10500073:                            /* WFI */
        wfi();
        return;
    }
    (void)rd;
    rv_illegal();
}

static void exec32(u32 i)
{
    u32 op = i & 0x7f, rd = (i >> 7) & 31, f3 = (i >> 12) & 7;
    u32 rs1 = (i >> 15) & 31, rs2 = (i >> 20) & 31, f7 = i >> 25;
    u64 a = rs->x[rs1], b = rs->x[rs2];
    s64 immi = (s32)i >> 20;
    switch (op) {
    case 0x37:                                  /* LUI */
        rv_wx(rd, (u64)(s64)(s32)(i & 0xfffff000u));
        return;
    case 0x17:                                  /* AUIPC */
        rv_wx(rd, rs->pc + (u64)(s64)(s32)(i & 0xfffff000u));
        return;
    case 0x6f: {                                /* JAL */
        s64 off = (s64)((s32)(i & 0x80000000u) >> 11) | (i & 0xff000u) |
                  ((i >> 9) & 0x800u) | ((i >> 20) & 0x7feu);
        u64 link = rs->npc;
        jump(rs->pc + (u64)off);
        rv_wx(rd, link);
        return;
    }
    case 0x67: {                                /* JALR */
        if (f3) {
            rv_illegal();
            return;
        }
        u64 link = rs->npc;
        jump((a + (u64)immi) & ~(u64)1);
        rv_wx(rd, link);
        return;
    }
    case 0x63: {                                /* branches */
        s64 off = (s64)((s32)(i & 0x80000000u) >> 19) | ((i << 4) & 0x800u) |
                  ((i >> 20) & 0x7e0u) | ((i >> 7) & 0x1eu);
        int t;
        switch (f3) {
        case 0: t = a == b; break;
        case 1: t = a != b; break;
        case 4: t = rv_sx(a) < rv_sx(b); break;
        case 5: t = rv_sx(a) >= rv_sx(b); break;
        case 6: t = a < b; break;
        case 7: t = a >= b; break;
        default: rv_illegal(); return;
        }
        if (t)
            jump(rs->pc + (u64)off);
        return;
    }
    case 0x03: {                                /* loads */
        u64 ad = rv_xl(a + (u64)immi), v;
        int n = 1 << (f3 & 3);
        if (f3 == 7 || (f3 == 3 && rs->xlen == 32) ||
            (f3 == 6 && rs->xlen == 32)) {
            rv_illegal();
            return;
        }
        v = rv_load(ad, n);
        if (rs->trap)
            return;
        if (!(f3 & 4) && n < 8)                 /* sign-extended */
            v = (u64)(n == 1 ? (s64)(int8_t)v : n == 2 ? (s64)(int16_t)v
                                                    : (s64)(s32)v);
        rv_wx(rd, v);
        return;
    }
    case 0x23: {                                /* stores */
        s64 imm = (s64)(((s32)i >> 25) << 5) | ((i >> 7) & 31);
        if (f3 > 3 || (f3 == 3 && rs->xlen == 32)) {
            rv_illegal();
            return;
        }
        rv_store(rv_xl(a + (u64)imm), 1 << f3, b);
        return;
    }
    case 0x13:
        op_imm(i, rd, f3, a, immi);
        return;
    case 0x33:
        op_reg(rd, f3, f7, a, b);
        return;
    case 0x1b:
        op_w(i, rd, f3, f7, a, b, 1);
        return;
    case 0x3b:
        op_w(i, rd, f3, f7, a, b, 0);
        return;
    case 0x0f:                                  /* FENCE, FENCE.I */
        if (f3 > 1)
            rv_illegal();
        return;
    case 0x73:
        sys_insn(i, rd, f3);
        return;
    case 0x2f:
        amo(i, rd, f3, a, b);
        return;
    }
    if (!rv_fp_exec(i))
        rv_illegal();
}

/* ---- the compressed instructions ---------------------------------------- */

static u32 enc_i(u32 op, u32 rd, u32 f3, u32 rs1, s32 imm)
{
    return ((u32)imm & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}

static u32 enc_s(u32 op, u32 f3, u32 rs1, u32 rs2, s32 imm)
{
    u32 m = (u32)imm;
    return ((m >> 5) & 0x7f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
           (m & 31) << 7 | op;
}

static u32 enc_r(u32 op, u32 rd, u32 f3, u32 rs1, u32 rs2, u32 f7)
{
    return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}

static u32 enc_b(u32 f3, u32 rs1, u32 rs2, s32 imm)
{
    u32 m = (u32)imm;
    return ((m >> 12) & 1) << 31 | ((m >> 5) & 0x3f) << 25 | rs2 << 20 |
           rs1 << 15 | f3 << 12 | ((m >> 1) & 15) << 8 |
           ((m >> 11) & 1) << 7 | 0x63;
}

static u32 enc_j(u32 rd, s32 imm)
{
    u32 m = (u32)imm;
    return ((m >> 20) & 1) << 31 | ((m >> 1) & 0x3ff) << 21 |
           ((m >> 11) & 1) << 20 | ((m >> 12) & 0xff) << 12 | rd << 7 | 0x6f;
}

static u32 bit(u32 h, int n)
{
    return (h >> n) & 1;
}

/* A compressed instruction's 32-bit equivalent, or 0 when it is not one
 * (reserved, or not at this width). */
static u32 rvc_expand(u32 h, int xlen)
{
    u32 f3 = h >> 13, rd = (h >> 7) & 31, rs2 = (h >> 2) & 31;
    u32 rdp = ((h >> 2) & 7) + 8, rs1p = ((h >> 7) & 7) + 8;
    /* the 6-bit signed immediate of CI forms */
    s32 ci = (s32)((bit(h, 12) << 5 | ((h >> 2) & 31)) << 26) >> 26;
    u32 uld = ((h >> 10) & 7) << 3 | ((h >> 5) & 3) << 6;      /* ld, fld */
    u32 ulw = ((h >> 10) & 7) << 3 | bit(h, 6) << 2 | bit(h, 5) << 6;
    switch (h & 3) {
    case 0:
        switch (f3) {
        case 0: {                               /* C.ADDI4SPN */
            u32 imm = ((h >> 11) & 3) << 4 | ((h >> 7) & 15) << 6 |
                      bit(h, 6) << 2 | bit(h, 5) << 3;
            if (!imm)
                return 0;
            return enc_i(0x13, rdp, 0, 2, (s32)imm);
        }
        case 1: return enc_i(0x07, rdp, 3, rs1p, (s32)uld);    /* C.FLD */
        case 2: return enc_i(0x03, rdp, 2, rs1p, (s32)ulw);    /* C.LW */
        case 3:
            return xlen == 32 ? enc_i(0x07, rdp, 2, rs1p, (s32)ulw) /* C.FLW */
                              : enc_i(0x03, rdp, 3, rs1p, (s32)uld); /* C.LD */
        case 5: return enc_s(0x27, 3, rs1p, rdp, (s32)uld);    /* C.FSD */
        case 6: return enc_s(0x23, 2, rs1p, rdp, (s32)ulw);    /* C.SW */
        case 7:
            return xlen == 32 ? enc_s(0x27, 2, rs1p, rdp, (s32)ulw) /* C.FSW */
                              : enc_s(0x23, 3, rs1p, rdp, (s32)uld); /* C.SD */
        }
        return 0;
    case 1:
        switch (f3) {
        case 0: return enc_i(0x13, rd, 0, rd, ci);             /* C.ADDI */
        case 1:
            if (xlen == 32)
                goto cj;                                       /* C.JAL */
            if (!rd)
                return 0;
            return enc_i(0x1b, rd, 0, rd, ci);                 /* C.ADDIW */
        case 2: return enc_i(0x13, rd, 0, 0, ci);              /* C.LI */
        case 3:
            if (rd == 2) {                                     /* C.ADDI16SP */
                s32 imm = (s32)((bit(h, 12) << 9 | bit(h, 6) << 4 |
                                 bit(h, 5) << 6 | ((h >> 3) & 3) << 7 |
                                 bit(h, 2) << 5) << 22) >> 22;
                if (!imm)
                    return 0;
                return enc_i(0x13, 2, 0, 2, imm);
            }
            if (!ci)
                return 0;
            return (u32)ci << 12 | rd << 7 | 0x37;             /* C.LUI */
        case 4: {
            u32 sh = bit(h, 12) << 5 | ((h >> 2) & 31);
            switch ((h >> 10) & 3) {
            case 0:                                            /* C.SRLI */
                if (xlen == 32 && bit(h, 12))
                    return 0;
                return enc_i(0x13, rs1p, 5, rs1p, (s32)sh);
            case 1:                                            /* C.SRAI */
                if (xlen == 32 && bit(h, 12))
                    return 0;
                return enc_i(0x13, rs1p, 5, rs1p, (s32)(sh | 0x400));
            case 2: return enc_i(0x13, rs1p, 7, rs1p, ci);     /* C.ANDI */
            }
            u32 f2 = (h >> 5) & 3;
            if (!bit(h, 12)) {
                static const u32 f3s[4] = { 0, 4, 6, 7 };      /* sub xor or and */
                return enc_r(0x33, rs1p, f3s[f2], rs1p, rdp, f2 ? 0 : 0x20);
            }
            if (xlen == 32 || f2 > 1)
                return 0;
            return enc_r(0x3b, rs1p, 0, rs1p, rdp, f2 ? 0 : 0x20); /* SUBW ADDW */
        }
        case 5:
        cj: {                                                  /* C.J, C.JAL */
            s32 imm = (s32)((bit(h, 12) << 11 | bit(h, 11) << 4 |
                             ((h >> 9) & 3) << 8 | bit(h, 8) << 10 |
                             bit(h, 7) << 6 | bit(h, 6) << 7 |
                             ((h >> 3) & 7) << 1 | bit(h, 2) << 5) << 20) >> 20;
            return enc_j(f3 == 1 ? 1 : 0, imm);
        }
        default: {                                             /* C.BEQZ C.BNEZ */
            s32 imm = (s32)((bit(h, 12) << 8 | ((h >> 10) & 3) << 3 |
                             ((h >> 5) & 3) << 6 | ((h >> 3) & 3) << 1 |
                             bit(h, 2) << 5) << 23) >> 23;
            return enc_b(f3 == 6 ? 0 : 1, rs1p, 0, imm);
        }
        }
    case 2: {
        u32 sld = bit(h, 12) << 5 | ((h >> 5) & 3) << 3 | ((h >> 2) & 7) << 6;
        u32 slw = bit(h, 12) << 5 | ((h >> 4) & 7) << 2 | ((h >> 2) & 3) << 6;
        u32 ssd = ((h >> 10) & 7) << 3 | ((h >> 7) & 7) << 6;
        u32 ssw = ((h >> 9) & 15) << 2 | ((h >> 7) & 3) << 6;
        switch (f3) {
        case 0: {                                              /* C.SLLI */
            u32 sh = bit(h, 12) << 5 | ((h >> 2) & 31);
            if (xlen == 32 && bit(h, 12))
                return 0;
            return enc_i(0x13, rd, 1, rd, (s32)sh);
        }
        case 1: return enc_i(0x07, rd, 3, 2, (s32)sld);        /* C.FLDSP */
        case 2:                                                /* C.LWSP */
            if (!rd)
                return 0;
            return enc_i(0x03, rd, 2, 2, (s32)slw);
        case 3:
            if (xlen == 32)
                return enc_i(0x07, rd, 2, 2, (s32)slw);        /* C.FLWSP */
            if (!rd)
                return 0;
            return enc_i(0x03, rd, 3, 2, (s32)sld);            /* C.LDSP */
        case 4:
            if (!bit(h, 12)) {
                if (!rs2) {
                    if (!rd)
                        return 0;
                    return enc_i(0x67, 0, 0, rd, 0);           /* C.JR */
                }
                return enc_r(0x33, rd, 0, 0, rs2, 0);          /* C.MV */
            }
            if (!rs2) {
                if (!rd)
                    return 0x00100073;                         /* C.EBREAK */
                return enc_i(0x67, 1, 0, rd, 0);               /* C.JALR */
            }
            return enc_r(0x33, rd, 0, rd, rs2, 0);             /* C.ADD */
        case 5: return enc_s(0x27, 3, 2, rs2, (s32)ssd);       /* C.FSDSP */
        case 6: return enc_s(0x23, 2, 2, rs2, (s32)ssw);       /* C.SWSP */
        default:
            return xlen == 32 ? enc_s(0x27, 2, 2, rs2, (s32)ssw)  /* C.FSWSP */
                              : enc_s(0x23, 3, 2, rs2, (s32)ssd); /* C.SDSP */
        }
    }
    }
    return 0;
}

/* ---- the run ------------------------------------------------------------ */

/* an interrupt taken before the next instruction: 1 if one was */
static int take_interrupt(void)
{
    if (rs->priv == PRV_M && !(rs->mstatus & MSTATUS_MIE))
        return 0;
    u64 p = rs->mie & rv_mip();
    if (!p)
        return 0;
    /* the order of the privileged architecture: MEI, MSI, MTI */
    int n = (p & (1u << 11)) ? 11 : (p & MIP_MSIP) ? IRQ_MSI : IRQ_MTI;
    trap_enter((u64)n, 0, 1);
    return 1;
}

/* the instruction's two halfwords from memory, or a device (the reset
 * ROM); 0 when there is nothing to fetch */
static int fetch16(u32 a, u32 *h)
{
    struct region *r = bus_region(&rs->sim->bus, a, 2);
    if (r) {
        const u8 *p = r->mem + (a - r->base);
        *h = (u32)p[0] | (u32)p[1] << 8;
        return 1;
    }
    return bus_read(&rs->sim->bus, a, 2, h) == 0;
}

static void step(struct cpu *c)
{
    rs = (struct rv_state *)c;
    struct sim *s = rs->sim;
    if (rs->mie && take_interrupt())
        return;
    u32 h0, h1 = 0;
    u32 pc = (u32)rs->pc;
    if ((rs->pc >> 32) || !fetch16(pc, &h0) ||
        ((h0 & 3) == 3 && !fetch16(pc + 2, &h1))) {
        if (rs->pc == (rs->mtvec & ~(u64)3)) {
            sim_end(s, END_LOCKUP, "lockup: the trap vector 0x%08llx cannot "
                    "be fetched", (unsigned long long)rs->pc);
            return;
        }
        trap_enter(EXC_IFETCH_ACCESS, rs->pc, 0);
        return;
    }
    int size = (h0 & 3) == 3 ? 4 : 2;
    u8 b[4] = { (u8)h0, (u8)(h0 >> 8), (u8)h1, (u8)(h1 >> 8) };
    int br;
    u32 cost = (u32)rv_cost(b, (size_t)size, &br);
    rs->cyc_now = s->cycles;
    rs->insn_now = s->insns;
    s->insns++;
    s->cycles += cost;
    u32 insn = size == 4 ? h0 | h1 << 16 : h0;
    if (s->trace)
        trace_insn(s, pc, &insn, 1, size);

    rs->npc = rv_xl(rs->pc + (u64)size);
    rs->trap = 0;
    rs->changed = 0;
    rs->insn = insn;
    if (size == 4) {
        exec32(insn);
    } else {
        u32 x = rvc_expand(insn, rs->xlen);
        if (x)
            exec32(x);
        else
            rv_illegal();
    }
    if (rs->trap) {
        if (rs->cause == TRAP_WATCH) {
            /* not run: the debugger stops here, and the instruction is
             * counted when it does run */
            s->insns--;
            s->cycles -= cost;
            return;
        }
        trap_enter(rs->cause, rs->tval, 0);
        return;
    }
    if (br && rs->npc != rs->pc + (u64)size)
        s->cycles += 2;
    if (s->counting)
        sim_advance(s, cost);
    if (rs->npc == rs->pc && !rs->changed && s->state == RUN) {
        /* a jump to itself that changed nothing: the end, unless an
         * interrupt can come */
        u32 left;
        int on = rs->priv < PRV_M || (rs->mstatus & MSTATUS_MIE);
        if (!on || !rs->mie ||
            (!(rs->mie & rv_mip()) && !sim_next_event(s, &left))) {
            sim_end(s, END_IDLE, "a loop at 0x%08llx that nothing can "
                    "interrupt", (unsigned long long)rs->pc);
            return;
        }
    }
    rs->pc = rs->npc;
}

/* ---- the CPU interface -------------------------------------------------- */

/* Power-on: the registers zero, machine mode, and the pc at QEMU's reset
 * vector. The width is the model's, or the image's. */
static void cpu_reset(struct cpu *c)
{
    struct rv_state *k = (struct rv_state *)c;
    struct cpu keep = k->cpu;
    struct sim *s = k->sim;
    int mx = k->model_xlen;
    memset(k, 0, sizeof *k);
    k->cpu = keep;
    k->sim = s;
    k->model_xlen = mx;
    k->xlen = mx ? mx : s->elf64 ? 64 : 32;
    if (mx && (mx == 64) != (s->elf64 != 0))
        die("%s is an RV%d image, and the core is RV%d", s->image,
            s->elf64 ? 64 : 32, mx);
    k->priv = PRV_M;
    k->pc = 0x1000;
    rs = k;
}

static u32 cpu_pc(struct cpu *c)
{
    return (u32)((struct rv_state *)c)->pc;
}

static void cpu_interrupt(struct cpu *c, int n)
{
    /* a software interrupt, as the CLINT's msip raises one */
    rs = (struct rv_state *)c;
    if (n == IRQ_MSI)
        rs->msip = 1;
}

/* ---- the registers, as GDB numbers them ---------------------------------
 *
 * QEMU's stub: x0-x31 are 0-31 and pc 32 (org.gnu.gdb.riscv.cpu), f0-f31
 * 33-64 at 64 bits (org.gnu.gdb.riscv.fpu, D being there), priv 65
 * (org.gnu.gdb.riscv.virtual), and CSR n is 66 + n
 * (org.gnu.gdb.riscv.csr). A `g` packet carries x0-x31 and pc. */

enum { GR_PC = 32, GR_F0 = 33, GR_PRIV = 65, GR_CSR = 66 };

static int put(u8 *b, u64 v, int n)
{
    for (int i = 0; i < n; i++)
        b[i] = (u8)(v >> (8 * i));
    return n;
}

static u64 get(const u8 *b, int n)
{
    u64 v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | b[i];
    return v;
}

static int cpu_reg_read(struct cpu *c, int n, u8 *buf)
{
    rs = (struct rv_state *)c;
    int w = rs->xlen / 8;
    u64 v;
    if (n >= 0 && n < 32)
        return put(buf, rs->x[n], w);
    if (n == GR_PC)
        return put(buf, rs->pc, w);
    if (n >= GR_F0 && n < GR_F0 + 32)
        return put(buf, rs->f[n - GR_F0], 8);
    if (n == GR_PRIV)
        return put(buf, (u64)rs->priv, w);
    if (n >= GR_CSR && n < GR_CSR + 4096 && csr_rd((u32)(n - GR_CSR), &v, 1))
        return put(buf, v, w);
    return 0;
}

static int cpu_reg_write(struct cpu *c, int n, const u8 *buf)
{
    rs = (struct rv_state *)c;
    int w = rs->xlen / 8;
    u64 v = get(buf, n >= GR_F0 && n < GR_F0 + 32 ? 8 : w), old;
    if (n >= 0 && n < 32) {
        if (n)
            rs->x[n] = rv_xl(v);
        return w;
    }
    if (n == GR_PC) {
        rs->pc = rv_xl(v) & ~(u64)1;
        return w;
    }
    if (n >= GR_F0 && n < GR_F0 + 32) {
        rs->f[n - GR_F0] = v;
        return 8;
    }
    if (n == GR_PRIV) {
        rs->priv = v == PRV_U ? PRV_U : PRV_M;
        return w;
    }
    if (n >= GR_CSR && n < GR_CSR + 4096) {
        u32 csr = (u32)(n - GR_CSR);
        if (!csr_rd(csr, &old, 1))
            return 0;
        u64 st = rs->mstatus;
        csr_wr(csr, v);
        if (csr <= 3)                       /* a debugger's write is not the program's */
            rs->mstatus = st;
        return w;
    }
    return 0;
}

static const int g_regs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                              14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                              25, 26, 27, 28, 29, 30, 31, GR_PC, -1 };

static const int *cpu_gdb_g_regs(struct cpu *c)
{
    (void)c;
    return g_regs;
}

/* The target description: as QEMU's stub, a target.xml that includes
 * the four features, each its own annex. */
static const char *const xnames[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "fp", "s1", "a0",
    "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6",
    "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };
static const char *const fnames[32] = {
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1",
    "fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7", "fs2", "fs3",
    "fs4", "fs5", "fs6", "fs7", "fs8", "fs9", "fs10", "fs11", "ft8", "ft9",
    "ft10", "ft11" };

static const char xml_note[] =
    "<?xml version=\"1.0\"?>\n"
    "<!-- Copyright (C) 2018-2019 Free Software Foundation, Inc.\n\n"
    "     Copying and distribution of this file, with or without modification,\n"
    "     are permitted in any medium without royalty provided the copyright\n"
    "     notice and this notice are preserved.  -->\n\n"
    "<!DOCTYPE feature SYSTEM \"gdb-target.dtd\">\n";

/* the CSRs the description lists: those the core has, by number */
static const struct { u32 n; const char *name; } csr_names[] = {
    { 0x001, "fflags" }, { 0x002, "frm" }, { 0x003, "fcsr" },
    { 0x300, "mstatus" }, { 0x301, "misa" }, { 0x302, "medeleg" },
    { 0x303, "mideleg" }, { 0x304, "mie" }, { 0x305, "mtvec" },
    { 0x306, "mcounteren" }, { 0x30a, "menvcfg" }, { 0x310, "mstatush" },
    { 0x31a, "menvcfgh" }, { 0x320, "mcountinhibit" },
    { 0x340, "mscratch" }, { 0x341, "mepc" }, { 0x342, "mcause" },
    { 0x343, "mtval" }, { 0x344, "mip" }, { 0x3a0, "pmpcfg0" },
    { 0x3a1, "pmpcfg1" }, { 0x3a2, "pmpcfg2" }, { 0x3a3, "pmpcfg3" },
    { 0x3b0, "pmpaddr0" }, { 0x3b1, "pmpaddr1" }, { 0x3b2, "pmpaddr2" },
    { 0x3b3, "pmpaddr3" }, { 0x3b4, "pmpaddr4" }, { 0x3b5, "pmpaddr5" },
    { 0x3b6, "pmpaddr6" }, { 0x3b7, "pmpaddr7" }, { 0xb00, "mcycle" },
    { 0xb02, "minstret" }, { 0xb80, "mcycleh" }, { 0xb82, "minstreth" },
    { 0xc00, "cycle" }, { 0xc01, "time" }, { 0xc02, "instret" },
    { 0xc80, "cycleh" }, { 0xc81, "timeh" }, { 0xc82, "instreth" },
    { 0xf11, "mvendorid" }, { 0xf12, "marchid" }, { 0xf13, "mimpid" },
    { 0xf14, "mhartid" }, { 0xf15, "mconfigptr" },
};

static char xml_buf[8192];

static void cat(const char *fmt, ...)
{
    size_t len = strlen(xml_buf);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(xml_buf + len, sizeof xml_buf - len, fmt, ap);
    va_end(ap);
}

static const char *cpu_gdb_xml(struct cpu *c, const char *annex)
{
    struct rv_state *k = (struct rv_state *)c;
    int w = k->xlen;
    char cpu[32], fpu[32], virt[32];
    snprintf(cpu, sizeof cpu, "riscv-%dbit-cpu.xml", w);
    snprintf(fpu, sizeof fpu, "riscv-64bit-fpu.xml");
    snprintf(virt, sizeof virt, "riscv-%dbit-virtual.xml", w);
    xml_buf[0] = 0;
    if (!strcmp(annex, "target.xml")) {
        cat("<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM "
            "\"gdb-target.dtd\"><target><architecture>riscv:rv%d"
            "</architecture><xi:include href=\"%s\"/><xi:include "
            "href=\"%s\"/><xi:include href=\"%s\"/><xi:include "
            "href=\"riscv-csr.xml\"/></target>", w, cpu, fpu, virt);
    } else if (!strcmp(annex, cpu)) {
        cat("%s<feature name=\"org.gnu.gdb.riscv.cpu\">\n", xml_note);
        for (int i = 0; i < 32; i++)
            cat("  <reg name=\"%s\" bitsize=\"%d\" type=\"%s\"/>\n", xnames[i],
                w, i == 1 ? "code_ptr" : (i >= 2 && i <= 4) || i == 8
                                             ? "data_ptr" : "int");
        cat("  <reg name=\"pc\" bitsize=\"%d\" type=\"code_ptr\"/>\n"
            "</feature>\n", w);
    } else if (!strcmp(annex, fpu)) {
        cat("%s<feature name=\"org.gnu.gdb.riscv.fpu\">\n\n"
            "  <union id=\"riscv_double\">\n"
            "    <field name=\"float\" type=\"ieee_single\"/>\n"
            "    <field name=\"double\" type=\"ieee_double\"/>\n"
            "  </union>\n\n", xml_note);
        for (int i = 0; i < 32; i++)
            cat("  <reg name=\"%s\" bitsize=\"64\" type=\"riscv_double\"/>\n",
                fnames[i]);
        cat("</feature>\n");
    } else if (!strcmp(annex, virt)) {
        cat("%s<feature name=\"org.gnu.gdb.riscv.virtual\">\n"
            "  <reg name=\"priv\" bitsize=\"%d\"/>\n</feature>\n", xml_note, w);
    } else if (!strcmp(annex, "riscv-csr.xml")) {
        cat("<?xml version=\"1.0\"?><!DOCTYPE feature SYSTEM "
            "\"gdb-target.dtd\"><feature name=\"org.gnu.gdb.riscv.csr\">");
        for (size_t i = 0; i < sizeof csr_names / sizeof csr_names[0]; i++) {
            u64 v;
            struct rv_state *save = rs;
            rs = k;
            int have = csr_rd(csr_names[i].n, &v, 1);
            rs = save;
            if (have)
                cat("<reg name=\"%s\" bitsize=\"%d\" regnum=\"%u\" "
                    "type=\"int\"/>", csr_names[i].name, w,
                    (unsigned)(GR_CSR + csr_names[i].n));
        }
        cat("</feature>");
    } else {
        return 0;
    }
    return xml_buf;
}


static const struct cpu_ops riscv_ops = {
    "riscv", 243, "a RISC-V image",
    cpu_reset, step, cpu_pc, cpu_reg_read, cpu_reg_write,
    cpu_gdb_g_regs, cpu_gdb_xml, 4, cpu_interrupt, GR_PC, 3,
};

int riscv_is(const struct cpu *c)
{
    return c && c->ops == &riscv_ops;
}

/* the core: rv32, rv64, or riscv (the width of the image it runs) */
struct cpu *riscv_create(struct sim *s, const char *model,
                         const struct board_desc *bd)
{
    int xlen;
    (void)bd;
    if (!strcmp(model, "rv32"))
        xlen = 32;
    else if (!strcmp(model, "rv64"))
        xlen = 64;
    else if (!strcmp(model, "riscv"))
        xlen = 0;
    else
        return 0;
    struct rv_state *k = calloc(1, sizeof *k);
    if (!k)
        die("out of memory");
    k->cpu.ops = &riscv_ops;
    k->sim = s;
    k->model_xlen = xlen;
    k->xlen = xlen ? xlen : 64;
    k->priv = PRV_M;
    rs = k;
    s->cpu = &k->cpu;
    return &k->cpu;
}
