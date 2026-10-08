/* The cycle model tools/bench charges per instruction, shared by the QEMU
 * plugin that counts a run (icount.c) and by EmbSim, which runs one
 * itself (tools/embsim). One table, so the two estimates cannot drift:
 * tests/golden/embsim.sh compares them over the exec corpus.
 *
 *   Cortex-M4 (the Technical Reference Manual's table, simplified): 1,
 *   a load 2, a load or store multiple (push, pop) 1 + the registers, a
 *   load or store pair 3, sdiv/udiv 7 (2 to 12 by the operands), vdiv
 *   and vsqrt 14; and a taken branch 2 more (the pipeline refill).
 *
 *   RV32 (no one core: a plain in-order pipeline): 1, a load 2 (flw and
 *   fld too, and their compressed forms), a divide or remainder 16, fdiv
 *   and fsqrt 16, and a taken branch or jump 2 more.
 *
 *   RV64: the same table, the same pipeline at 64 bits -- ld and c.ld are
 *   loads, and divw, divuw, remw and remuw divides like the rest; a
 *   64-bit divide is charged as a 32-bit one (a model of the choices a
 *   compiler makes, not of one core).
 *
 *   AVR (the ATmega328P's datasheet, "Instruction Set Summary": its
 *   cycles are exact and documented, for a part with a 16-bit PC and no
 *   wait states): 1, and 2 for ADIW, SBIW, the multiplies, the loads and
 *   stores, PUSH, POP, SBI, CBI, RJMP and IJMP; 3 for LPM, ELPM, JMP,
 *   RCALL and ICALL; 4 for CALL, RET and RETI. A conditional branch
 *   taken is 1 more, not 2; a skip (CPSE, SBRC, SBRS, SBIC, SBIS) 1 more
 *   when it skips a one-word instruction and 2 more for a two-word one,
 *   which EmbSim adds and QEMU's plugin cannot see.
 *
 * Each function takes an instruction's bytes (2 or 4) and returns its
 * cost, and says whether it may branch; the caller adds the cycles of a
 * taken branch (2; 1 on the AVR) when execution does not continue at
 * the next one. */
#ifndef EMB_BENCH_COST_H
#define EMB_BENCH_COST_H
#include <stddef.h>
#include <stdint.h>

static inline int popcount16(unsigned v)
{
    int n = 0;
    for (v &= 0xffff; v; v &= v - 1)
        n++;
    return n;
}

/* A Thumb instruction's cost and whether it may branch. */
static inline int arm_cost(const uint8_t *p, size_t n, int *branch)
{
    unsigned h1 = p[0] | p[1] << 8;
    *branch = 0;
    if (n == 2) {
        if ((h1 & 0xff00) == 0x4700) {                  /* bx, blx */
            *branch = 1;
            return 1;
        }
        if ((h1 & 0xf800) == 0x4800)                    /* ldr literal */
            return 2;
        if ((h1 & 0xf000) == 0x5000)                    /* ld/st register */
            return ((h1 >> 9) & 7) >= 3 ? 2 : 1;
        if ((h1 & 0xe000) == 0x6000 || (h1 & 0xf000) == 0x8000 ||
            (h1 & 0xf000) == 0x9000)                    /* ld/st immediate */
            return h1 & 0x0800 ? 2 : 1;
        if ((h1 & 0xfe00) == 0xb400)                    /* push */
            return 1 + popcount16(h1 & 0x1ff);
        if ((h1 & 0xfe00) == 0xbc00) {                  /* pop */
            *branch = (h1 & 0x100) != 0;
            return 1 + popcount16(h1 & 0x1ff);
        }
        if ((h1 & 0xf000) == 0xc000)                    /* stm, ldm */
            return 1 + popcount16(h1 & 0xff);
        if ((h1 & 0xf500) == 0xb100) {                  /* cbz, cbnz */
            *branch = 1;
            return 1;
        }
        if ((h1 & 0xf000) == 0xd000 && (h1 & 0x0e00) != 0x0e00) {
            *branch = 1;                                /* b<cond> */
            return 1;
        }
        if ((h1 & 0xf800) == 0xe000) {                  /* b */
            *branch = 1;
            return 1;
        }
        return 1;
    }
    unsigned h2 = p[2] | p[3] << 8;
    if ((h1 & 0xf800) == 0xf000 && (h2 & 0x8000)) {
        /* b, bl and the miscellaneous control space; only the branches
         * have bit 12 or a condition outside 111x */
        if ((h2 & 0x5000) || (h1 & 0x0380) != 0x0380)
            *branch = 1;
        return 1;
    }
    if ((h1 & 0xfff0) == 0xe8d0 && (h2 & 0xffe0) == 0xf000) {
        *branch = 1;                                    /* tbb, tbh */
        return 2;
    }
    if ((h1 & 0xfe40) == 0xe800) {                      /* ldm, stm */
        if ((h1 & 0x0010) && (h2 & 0x8000))
            *branch = 1;                                /* pop.w {.., pc} */
        return 1 + popcount16(h2);
    }
    if ((h1 & 0xfff0) == 0xe850)                        /* ldrex */
        return 2;
    if ((h1 & 0xfe40) == 0xe840 && (h1 & 0x0120))       /* ldrd, strd */
        return 3;
    if ((h1 & 0xfe00) == 0xf800) {                      /* ld/st single */
        if ((h1 & 0x0010) && (h2 & 0xf000) == 0xf000 &&
            ((h1 >> 5) & 3) == 2)
            *branch = 1;                                /* ldr pc */
        return h1 & 0x0010 ? 2 : 1;
    }
    if ((h1 & 0xffd0) == 0xfb90)                        /* sdiv, udiv */
        return 7;
    if ((h1 & 0xfe10) == 0xec10 && (h1 & 0x0100))       /* vldr, vldm */
        return 2;
    if ((h1 & 0xffb0) == 0xee80 && (h2 & 0x0e50) == 0x0a00)
        return 14;                                      /* vdiv */
    if ((h1 & 0xffbf) == 0xeeb1 && (h2 & 0x0ed0) == 0x0ac0)
        return 14;                                      /* vsqrt */
    return 1;
}

/* A RISC-V instruction's cost and whether it may branch (with C). */
static inline int rv_cost(const uint8_t *p, size_t n, int *branch)
{
    *branch = 0;
    if (n == 2) {
        unsigned h = p[0] | p[1] << 8, q = h & 3, f3 = h >> 13;
        if (q == 0)                                 /* c.fld c.lw c.flw/c.ld */
            return f3 >= 1 && f3 <= 3 ? 2 : 1;
        if (q == 1) {
            if (f3 == 1 || f3 == 5 || f3 == 6 || f3 == 7)
                *branch = 1;                            /* c.jal c.j c.b*z */
            return 1;
        }
        if (f3 >= 1 && f3 <= 3)                         /* c.fldsp c.lwsp */
            return 2;                                   /* c.flwsp/c.ldsp */
        if (f3 == 4 && ((h >> 2) & 0x1f) == 0 && ((h >> 7) & 0x1f))
            *branch = 1;                                /* c.jr, c.jalr */
        return 1;
    }
    uint32_t w = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    unsigned op = w & 0x7f;
    if (op == 0x03 || op == 0x07)                       /* loads */
        return 2;
    if (op == 0x63 || op == 0x6f || op == 0x67) {       /* b*, jal, jalr */
        *branch = 1;
        return 1;
    }
    if ((op == 0x33 || op == 0x3b) && (w >> 25) == 1 && ((w >> 12) & 7) >= 4)
        return 16;                                      /* div, rem (and w) */
    if (op == 0x53 && ((w >> 27) == 0x03 || (w >> 27) == 0x0b))
        return 16;                                      /* fdiv, fsqrt */
    return 1;
}

/* An AVR instruction's cycles and whether it is a conditional branch
 * (BRBS, BRBC: 1 more when taken). */
static inline int avr_cost(const uint8_t *p, size_t n, int *branch)
{
    unsigned w = p[0] | p[1] << 8;
    (void)n;
    *branch = 0;
    switch (w >> 12) {
    case 0x0:
        return (w & 0xfe00) == 0x0200 ? 2 : 1;          /* muls, mulsu, fmul* */
    case 0x8: case 0xa:
        return 2;                                       /* ldd, std */
    case 0x9:
        break;
    case 0xc:
        return 2;                                       /* rjmp */
    case 0xd:
        return 3;                                       /* rcall */
    case 0xf:
        if (!(w & 0x0800))
            *branch = 1;                                /* brbs, brbc */
        return 1;
    default:
        return 1;
    }
    switch ((w >> 8) & 0xf) {
    case 0x0: case 0x1:                                 /* lds ld lpm elpm pop */
        return (w & 0xc) == 0x4 ? 3 : 2;
    case 0x2: case 0x3:                                 /* sts st push */
        return 2;
    case 0x4: case 0x5:
        if ((w & 0xfe0e) == 0x940c)
            return 3;                                   /* jmp */
        if ((w & 0xfe0e) == 0x940e)
            return 4;                                   /* call */
        if ((w & 0xf) < 8 || (w & 0xf) == 0xa || (w & 0xff0f) == 0x9408)
            return 1;                                   /* one register, bset, bclr */
        switch (w) {
        case 0x9508: case 0x9518: case 0x9519:          /* ret reti eicall */
            return 4;
        case 0x95c8: case 0x95d8: case 0x9509:          /* lpm elpm icall */
            return 3;
        case 0x9409: case 0x9419:                       /* ijmp eijmp */
            return 2;
        }
        return 1;                                       /* sleep break wdr spm */
    case 0x6: case 0x7: case 0x8: case 0xa:             /* adiw sbiw cbi sbi */
        return 2;
    case 0x9: case 0xb:                                 /* sbic sbis */
        return 1;
    default:                                            /* mul */
        return 2;
    }
}

#endif
