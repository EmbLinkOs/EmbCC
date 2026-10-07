/* embsim -- a Cortex-M simulator: run a firmware image without a board.
 *
 * It runs an ELF image the way the part does. The core fetches its stack
 * pointer and reset vector from the vector table, and executes Thumb
 * (ARMv6-M, or ARMv7-M with the FPv4-SP or FPv5 floating-point unit).
 * Exceptions are architectural: fault escalation and lockup, SVC, PendSV,
 * SysTick and the NVIC's interrupts, with the stacked frame and
 * EXC_RETURN. The boards are the ones QEMU models, so an image built for
 * QEMU runs unchanged, and QEMU referees the simulator: tests/golden/
 * embsim.sh runs the exec corpus on both, and the output and the
 * instruction count must be the same.
 *
 * It counts the instructions it executes, and estimates their cycles with
 * the table tools/bench uses (tools/bench/cost.h). SysTick and DWT's
 * CYCCNT advance by that estimate, so a timed run gives the same answer
 * every time. QEMU's SysTick follows the host's clock and does not.
 *
 * Output comes from the board's UART, or from semihosting (`bkpt 0xab`),
 * which can also end the run with an exit status. A run otherwise ends
 * when:
 *   - the core locks up (a fault with nowhere to go);
 *   - it waits in a loop or a WFI that nothing can interrupt;
 *   - the output contains --until's string;
 *   - --max-insns runs out.
 *
 * The C is ISO C99 with no dependencies, so it builds on a machine with
 * no QEMU (the portable-host goal: docs/manual/tools/embsim.md). */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bench/cost.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

static void die(const char *fmt, ...)
{
    va_list ap;
    fputs("embsim: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

/* ---- the boards ----------------------------------------------------- */

enum { UART_PL011, UART_CMSDK, UART_NRF51 };
enum { ARCH_V6M, ARCH_V7M };
enum { FPU_NONE, FPU_SP, FPU_DP };

struct cpu_model {
    const char *name;
    int arch, dsp, fpu;
    u32 cpuid;
};

static const struct cpu_model cpus[] = {
    { "cortex-m0", ARCH_V6M, 0, FPU_NONE, 0x410CC200u },
    { "cortex-m0plus", ARCH_V6M, 0, FPU_NONE, 0x410CC601u },
    { "cortex-m3", ARCH_V7M, 0, FPU_NONE, 0x412FC231u },
    { "cortex-m4", ARCH_V7M, 1, FPU_SP, 0x410FC241u },
    { "cortex-m7", ARCH_V7M, 1, FPU_DP, 0x411FC272u },
};

struct board {
    const char *name, *cpu;
    u32 flash, flash_size, ram, ram_size, ram2, ram2_size;
    int uart;
    u32 uart_base;
    int prio_bits, bitband, flash_rom;
};

/* The memory and UART of QEMU's model of each board, so an image linked
 * for one runs under either. The lm3s6965 and the nRF51 have flash at 0,
 * which a store does not change; the MPS2 boards have SSRAM there. */
static const struct board boards[] = {
    { "lm3s6965evb", "cortex-m3", 0, 256u << 10, 0x20000000u, 64u << 10,
      0, 0, UART_PL011, 0x4000C000u, 3, 1, 1 },
    { "mps2-an385", "cortex-m3", 0, 4u << 20, 0x20000000u, 4u << 20,
      0x60000000u, 16u << 20, UART_CMSDK, 0x40004000u, 3, 1, 0 },
    { "mps2-an386", "cortex-m4", 0, 4u << 20, 0x20000000u, 4u << 20,
      0x60000000u, 16u << 20, UART_CMSDK, 0x40004000u, 3, 1, 0 },
    { "mps2-an500", "cortex-m7", 0, 4u << 20, 0x20000000u, 4u << 20,
      0x60000000u, 16u << 20, UART_CMSDK, 0x40004000u, 3, 1, 0 },
    { "microbit", "cortex-m0", 0, 256u << 10, 0x20000000u, 16u << 10,
      0, 0, UART_NRF51, 0x40002000u, 2, 0, 1 },
};

static const struct board *bd;
static const struct cpu_model *cm;

/* ---- options and the run's end -------------------------------------- */

static const char *until;           /* stop once the output holds this */
static size_t until_len, until_at;  /* how much of it has matched */
static u64 max_insns;
static int trace, semihosting = 1, verbose;
static FILE *trace_out;

enum { RUN, END_EXIT, END_LOCKUP, END_IDLE, END_UNTIL, END_BUDGET };
static int state = RUN, exit_status;
static char end_why[160];

static u64 insns, cycles;

static void out_byte(int c)
{
    putchar(c);
    if (until) {
        /* a plain prefix automaton is enough for the sentinels harnesses
         * print: restart the match at this byte when it breaks */
        if ((u8)until[until_at] == (u8)c)
            until_at++;
        else
            until_at = (u8)until[0] == (u8)c;
        if (until_at == until_len && state == RUN) {
            state = END_UNTIL;
            snprintf(end_why, sizeof end_why, "the output reached \"%s\"",
                     until);
        }
    }
}

/* ---- memory ---------------------------------------------------------- */

struct region {
    u32 base, size;
    u8 *mem;
    int rom;
};
static struct region rg[8];
static int nrg;

static void add_region(u32 base, u32 size, int rom)
{
    if (!size)
        return;
    if (nrg == 8)
        die("too many memory regions");
    rg[nrg].base = base;
    rg[nrg].size = size;
    rg[nrg].rom = rom;
    rg[nrg].mem = calloc(size, 1);
    if (!rg[nrg].mem)
        die("out of memory for %u bytes at 0x%08x", size, base);
    nrg++;
}

static struct region *region_of(u32 a, u32 n)
{
    for (int i = 0; i < nrg; i++)
        if (a - rg[i].base < rg[i].size && rg[i].size - (a - rg[i].base) >= n)
            return &rg[i];
    return 0;
}

/* A fault raised by the instruction executing: the first one wins, and
 * every access after it is suppressed, so the instruction's partial work
 * is discarded with the register snapshot (step()). */
enum { EXC_RESET = 1, EXC_NMI, EXC_HARD, EXC_MEM, EXC_BUS, EXC_USAGE,
       EXC_SVC = 11, EXC_DEBUG, EXC_PENDSV = 14, EXC_SYSTICK };
static int fault_exc;
static u32 fault_bits, fault_addr;
static int fault_addr_valid;

/* CFSR bits */
#define UFSR_UNDEFINSTR (1u << 16)
#define UFSR_INVSTATE   (1u << 17)
#define UFSR_INVPC      (1u << 18)
#define UFSR_NOCP       (1u << 19)
#define UFSR_UNALIGNED  (1u << 24)
#define UFSR_DIVBYZERO  (1u << 25)
#define BFSR_IBUSERR    (1u << 8)
#define BFSR_PRECISERR  (1u << 9)

static void fault_raise(int exc, u32 bits)
{
    if (fault_exc)
        return;
    /* ARMv6-M has no configurable faults: each is a HardFault */
    if (cm->arch == ARCH_V6M && exc >= EXC_MEM && exc <= EXC_USAGE)
        exc = EXC_HARD;
    fault_exc = exc;
    fault_bits = bits;
}

static void bus_error(u32 a, int ifetch)
{
    if (fault_exc)
        return;
    fault_raise(EXC_BUS, ifetch ? BFSR_IBUSERR : BFSR_PRECISERR);
    fault_addr = a;
    fault_addr_valid = !ifetch;
}

/* ---- the system control space and the peripherals -------------------- */

#define NEXC 256                    /* 16 system exceptions + 240 IRQs */
static u8 pend[NEXC], active[NEXC], irq_en[NEXC];
static u8 prio[NEXC];               /* as programmed, top bits significant */
static int npend;
static u32 vtor, aircr_prigroup, ccr, shcsr, cfsr, hfsr, mmfar, bfar, scr;
static u32 cpacr, fpccr = 0xC0000000u, fpcar, fpdscr;
static u32 st_csr, st_rvr, st_cvr;
static u32 demcr, dwt_ctrl, dwt_cyccnt_base;
static u32 uart_ctrl;               /* CMSDK CTRL, or nRF51 ENABLE */
static int uart_tx_started;

/* CPU state */
static u32 R[16];                   /* R[13] is the active stack pointer */
static u32 other_sp;                /* the banked one */
static int psp_active;              /* R[13] is PSP */
static int N, Z, C, V, Q;
static u32 GE;
static u32 itstate;
static int tbit = 1;
static u32 ipsr;                    /* 0 in Thread mode */
static u32 primask, faultmask, basepri, control;
static u32 S[64];                   /* s0..s31 (d0..d15), and d16..d31 */
static u32 fpscr;
static int excl_valid;
static u32 excl_addr;

static u32 pc, npc;                 /* this instruction, and the next */
static int pc_written;
static u32 exc_return_value;
static int exc_return_pending;

static void set_pend(int n)
{
    if (n > 0 && n < NEXC && !pend[n]) {
        pend[n] = 1;
        npend++;
    }
}

static void clr_pend(int n)
{
    if (n > 0 && n < NEXC && pend[n]) {
        pend[n] = 0;
        npend--;
    }
}

static u32 prio_mask(void)
{
    return (0xffu << (8 - bd->prio_bits)) & 0xffu;
}

/* An exception's group priority, as a number to compare: lower is more
 * urgent; Reset, NMI and HardFault have the fixed -3, -2 and -1. */
static int group_prio(int n)
{
    if (n == EXC_RESET)
        return -3;
    if (n == EXC_NMI)
        return -2;
    if (n == EXC_HARD)
        return -1;
    u32 p = prio[n] & prio_mask();
    u32 sub = cm->arch == ARCH_V6M ? 0 : aircr_prigroup + 1;
    if (sub > 8)
        sub = 8;
    return (int)((p >> sub) << sub);
}

static int exec_prio(void)
{
    int best = 256;
    for (int n = 1; n < NEXC; n++)
        if (active[n] && group_prio(n) < best)
            best = group_prio(n);
    if (cm->arch == ARCH_V7M && (basepri & prio_mask())) {
        u32 sub = aircr_prigroup + 1;
        int b = sub > 7 ? 0 : (int)(((basepri & prio_mask()) >> sub) << sub);
        if (b < best)
            best = b;
    }
    if (primask & 1)
        best = best < 0 ? best : 0;
    if (faultmask & 1)
        best = best < -1 ? best : -1;
    return best;
}

static u32 icsr_read(void)
{
    u32 v = ipsr & 0x1ff;
    int pending = 0;
    for (int n = 2; n < NEXC; n++)
        if (pend[n]) {
            pending = n;
            break;
        }
    v |= (u32)pending << 12;
    if (pend[EXC_PENDSV])
        v |= 1u << 28;
    if (pend[EXC_SYSTICK])
        v |= 1u << 26;
    if (pend[EXC_NMI])
        v |= 1u << 31;
    int nact = 0;
    for (int n = 1; n < NEXC; n++)
        nact += active[n];
    if (nact <= 1)
        v |= 1u << 11;              /* RETTOBASE */
    return v;
}

static int scs_read(u32 a, u32 *v)
{
    u32 o = a - 0xE000E000u;
    *v = 0;
    if (o >= 0x100 && o < 0x140) {          /* ISER */
        u32 w = (o - 0x100) / 4;
        for (int b = 0; b < 32; b++)
            if (16 + w * 32 + b < NEXC && irq_en[16 + w * 32 + b])
                *v |= 1u << b;
        return 1;
    }
    if (o >= 0x180 && o < 0x1c0) {          /* ICER */
        return scs_read(a - 0x80, v);
    }
    if ((o >= 0x200 && o < 0x240) || (o >= 0x280 && o < 0x2c0)) {
        u32 w = (o & 0x7f) / 4;             /* ISPR, ICPR */
        for (int b = 0; b < 32; b++)
            if (16 + w * 32 + b < NEXC && pend[16 + w * 32 + b])
                *v |= 1u << b;
        return 1;
    }
    if (o >= 0x300 && o < 0x340) {          /* IABR */
        u32 w = (o - 0x300) / 4;
        for (int b = 0; b < 32; b++)
            if (16 + w * 32 + b < NEXC && active[16 + w * 32 + b])
                *v |= 1u << b;
        return 1;
    }
    if (o >= 0x400 && o < 0x400 + NEXC - 16) {  /* IPR, by word */
        u32 n = 16 + (o - 0x400);
        for (int b = 0; b < 4; b++)
            if (n + b < NEXC)
                *v |= (u32)(prio[n + b] & prio_mask()) << (8 * b);
        return 1;
    }
    switch (o) {
    case 0x004: *v = 7; return 1;           /* ICTR: 256 lines */
    case 0x010: *v = st_csr; st_csr &= ~(1u << 16); return 1;
    case 0x014: *v = st_rvr; return 1;
    case 0x018: *v = st_cvr; return 1;
    case 0x01c: *v = 0x80000000u; return 1; /* CALIB: no reference */
    case 0xd00: *v = cm->cpuid; return 1;
    case 0xd04: *v = icsr_read(); return 1;
    case 0xd08: *v = vtor; return 1;
    case 0xd0c: *v = 0xFA050000u | aircr_prigroup << 8; return 1;
    case 0xd10: *v = scr; return 1;
    case 0xd14: *v = ccr; return 1;
    case 0xd18: case 0xd1c: case 0xd20: {
        u32 n = 4 + (o - 0xd18);
        for (int b = 0; b < 4; b++)
            *v |= (u32)(prio[n + b] & prio_mask()) << (8 * b);
        return 1;
    }
    case 0xd24: *v = shcsr; return 1;
    case 0xd28: *v = cfsr; return 1;
    case 0xd2c: *v = hfsr; return 1;
    case 0xd34: *v = mmfar; return 1;
    case 0xd38: *v = bfar; return 1;
    case 0xd88: *v = cm->fpu ? cpacr : 0; return 1;
    case 0xdfc: *v = demcr; return 1;
    case 0xf34: *v = fpccr; return 1;
    case 0xf38: *v = fpcar; return 1;
    case 0xf3c: *v = fpdscr; return 1;
    }
    return 1;                               /* reads as zero */
}

static void scs_write(u32 a, u32 v)
{
    u32 o = a - 0xE000E000u;
    if (o >= 0x100 && o < 0x140) {
        u32 w = (o - 0x100) / 4;
        for (int b = 0; b < 32; b++)
            if ((v >> b & 1) && 16 + w * 32 + b < NEXC)
                irq_en[16 + w * 32 + b] = 1;
        return;
    }
    if (o >= 0x180 && o < 0x1c0) {
        u32 w = (o - 0x180) / 4;
        for (int b = 0; b < 32; b++)
            if ((v >> b & 1) && 16 + w * 32 + b < NEXC)
                irq_en[16 + w * 32 + b] = 0;
        return;
    }
    if (o >= 0x200 && o < 0x240) {
        u32 w = (o - 0x200) / 4;
        for (int b = 0; b < 32; b++)
            if (v >> b & 1)
                set_pend(16 + w * 32 + b);
        return;
    }
    if (o >= 0x280 && o < 0x2c0) {
        u32 w = (o - 0x280) / 4;
        for (int b = 0; b < 32; b++)
            if (v >> b & 1)
                clr_pend(16 + w * 32 + b);
        return;
    }
    if (o >= 0x400 && o < 0x400 + NEXC - 16) {
        u32 n = 16 + (o - 0x400);
        for (int b = 0; b < 4; b++)
            if (n + b < NEXC)
                prio[n + b] = (u8)(v >> (8 * b));
        return;
    }
    switch (o) {
    case 0x010:
        st_csr = (st_csr & (1u << 16)) | (v & 7);
        return;
    case 0x014: st_rvr = v & 0xffffff; return;
    case 0x018: st_cvr = 0; st_csr &= ~(1u << 16); return;
    case 0xd04:
        if (v & (1u << 31)) set_pend(EXC_NMI);
        if (v & (1u << 28)) set_pend(EXC_PENDSV);
        if (v & (1u << 27)) clr_pend(EXC_PENDSV);
        if (v & (1u << 26)) set_pend(EXC_SYSTICK);
        if (v & (1u << 25)) clr_pend(EXC_SYSTICK);
        return;
    case 0xd08: vtor = v & 0xffffff80u; return;
    case 0xd0c:
        if ((v >> 16) != 0x05FA)
            return;
        if (cm->arch == ARCH_V7M)
            aircr_prigroup = (v >> 8) & 7;
        if (v & 4) {                        /* SYSRESETREQ */
            state = END_EXIT;
            exit_status = 0;
            snprintf(end_why, sizeof end_why, "the image requested a reset");
        }
        return;
    case 0xd10: scr = v & 0x16; return;
    case 0xd14: ccr = (ccr & ~0x31Bu) | (v & 0x31Bu); return;
    case 0xd18: case 0xd1c: case 0xd20: {
        u32 n = 4 + (o - 0xd18);
        for (int b = 0; b < 4; b++)
            prio[n + b] = (u8)(v >> (8 * b));
        return;
    }
    case 0xd24: shcsr = v & 0x7ffffu; return;
    case 0xd28: cfsr &= ~v; return;
    case 0xd2c: hfsr &= ~v; return;
    case 0xd88: cpacr = v & 0x00f00000u; return;
    case 0xdfc: demcr = v; return;
    case 0xf00: set_pend(16 + (int)(v & 0x1ff)); return;   /* STIR */
    case 0xf34: fpccr = (fpccr & ~0xC0000000u) | (v & 0xC0000000u); return;
    case 0xf38: fpcar = v & ~7u; return;
    case 0xf3c: fpdscr = v & 0x07C00000u; return;
    }
}

/* DWT: only the cycle counter, from the cost model's estimate */
static int dwt_read(u32 a, u32 *v)
{
    *v = 0;
    if (a == 0xE0001000u)
        *v = dwt_ctrl;
    else if (a == 0xE0001004u)
        *v = (dwt_ctrl & 1) ? (u32)cycles - dwt_cyccnt_base : dwt_cyccnt_base;
    return 1;
}

static void dwt_write(u32 a, u32 v)
{
    if (a == 0xE0001000u) {
        u32 now = (dwt_ctrl & 1) ? (u32)cycles - dwt_cyccnt_base
                                 : dwt_cyccnt_base;
        dwt_ctrl = v & 1;
        /* keep CYCCNT where it was across the switch */
        dwt_cyccnt_base = (dwt_ctrl & 1) ? (u32)cycles - now : now;
    } else if (a == 0xE0001004u) {
        dwt_cyccnt_base = (dwt_ctrl & 1) ? (u32)cycles - v : v;
    }
}

static int uart_read(u32 o, u32 *v)
{
    *v = 0;
    switch (bd->uart) {
    case UART_PL011:
        if (o == 0x18)
            *v = 0x90;                      /* FR: TX empty, RX empty */
        break;
    case UART_CMSDK:
        if (o == 8)
            *v = uart_ctrl;
        break;
    case UART_NRF51:
        if (o == 0x11c)
            *v = 1;                         /* EVENTS_TXDRDY */
        break;
    }
    return 1;
}

static void uart_write(u32 o, u32 v)
{
    switch (bd->uart) {
    case UART_PL011:
        if (o == 0)
            out_byte((int)(v & 0xff));
        break;
    case UART_CMSDK:
        if (o == 0 && (uart_ctrl & 1))
            out_byte((int)(v & 0xff));
        else if (o == 8)
            uart_ctrl = v;
        break;
    case UART_NRF51:
        /* TXD sends only once ENABLE is 4 and STARTTX has run */
        if (o == 0x008)
            uart_tx_started = 1;
        else if (o == 0x00c)
            uart_tx_started = 0;
        else if (o == 0x500)
            uart_ctrl = v;
        else if (o == 0x51c && uart_ctrl == 4 && uart_tx_started)
            out_byte((int)(v & 0xff));
        break;
    }
}

/* A device read or write: 1 when the address is a device's (the access
 * is done), 0 when it is not. The peripheral space reads as zero and
 * ignores writes where nothing is modelled, as a part's reserved
 * registers mostly do. */
static int dev_access(u32 a, int n, u32 *v, int write)
{
    if (a >= bd->uart_base && a < bd->uart_base + 0x1000u) {
        if (write)
            uart_write(a - bd->uart_base, *v);
        else
            uart_read(a - bd->uart_base, v);
        return 1;
    }
    if (a >= 0xE000E000u && a < 0xE000F000u) {
        u32 o = a & 0xfff;
        if (!write) {
            scs_read(a & ~3u, v);
            *v >>= 8 * (a & 3);
        } else if (n == 4)
            scs_write(a, *v);
        else if ((o >= 0x400 && o < 0x5f0) || (o >= 0xd18 && o < 0xd24)) {
            /* the priority registers are byte-accessible: a CMSIS
             * NVIC_SetPriority writes one byte */
            for (int k = 0; k < n; k++)
                prio[(o >= 0xd18 ? 4 + (o - 0xd18) : 16 + (o - 0x400)) + k] =
                    (u8)(*v >> (8 * k));
        } else
            scs_write(a & ~3u, *v << (8 * (a & 3)));
        return 1;
    }
    if (a >= 0xE0001000u && a < 0xE0002000u) {
        if (write)
            dwt_write(a, *v);
        else
            dwt_read(a, v);
        return 1;
    }
    if ((a >= 0x40000000u && a < 0x60000000u) || a >= 0xE0000000u) {
        if (!write)
            *v = 0;
        return 1;
    }
    return 0;
}

static u32 rd_le(const u8 *p, int n)
{
    u32 v = p[0];
    if (n > 1)
        v |= (u32)p[1] << 8;
    if (n > 2)
        v |= (u32)p[2] << 16 | (u32)p[3] << 24;
    return v;
}

static void wr_le(u8 *p, int n, u32 v)
{
    p[0] = (u8)v;
    if (n > 1)
        p[1] = (u8)(v >> 8);
    if (n > 2) {
        p[2] = (u8)(v >> 16);
        p[3] = (u8)(v >> 24);
    }
}

/* the bit-band aliases of SRAM (0x22000000) and of the peripherals
 * (0x42000000): one word per bit */
static int bitband(u32 a, u32 *target, int *bit)
{
    if (!bd->bitband)
        return 0;
    if (a >= 0x22000000u && a < 0x24000000u) {
        *target = 0x20000000u + ((a - 0x22000000u) >> 5);
        *bit = (int)((a >> 2) & 7);
        return 1;
    }
    if (a >= 0x42000000u && a < 0x44000000u) {
        *target = 0x40000000u + ((a - 0x42000000u) >> 5);
        *bit = (int)((a >> 2) & 7);
        return 1;
    }
    return 0;
}

static u32 ld(u32 a, int n);
static void st(u32 a, int n, u32 v);

static u32 ld(u32 a, int n)
{
    if (fault_exc)
        return 0;
    struct region *r = region_of(a, (u32)n);
    if (r)
        return rd_le(r->mem + (a - r->base), n);
    u32 t, v;
    int b;
    if (bitband(a, &t, &b))
        return (ld(t, 1) >> b) & 1;
    if (dev_access(a, n, &v, 0))
        return n == 4 ? v : v & ((1u << (8 * n)) - 1);
    bus_error(a, 0);
    return 0;
}

static void st(u32 a, int n, u32 v)
{
    if (fault_exc)
        return;
    if (excl_valid && a - excl_addr < 4)
        excl_valid = 0;
    struct region *r = region_of(a, (u32)n);
    if (r) {
        if (!r->rom)
            wr_le(r->mem + (a - r->base), n, v);
        return;
    }
    u32 t;
    int b;
    if (bitband(a, &t, &b)) {
        u32 byte = ld(t, 1);
        st(t, 1, (v & 1) ? byte | 1u << b : byte & ~(1u << b));
        return;
    }
    if (dev_access(a, n, &v, 1))
        return;
    bus_error(a, 0);
}

/* An access an instruction makes: v6-M faults on every unaligned one;
 * v7-M on the ones that must be aligned (multiple, dual, exclusive),
 * and on the rest only with CCR.UNALIGN_TRP. */
static int aligned_ok(u32 a, int n, int must)
{
    if ((a & (u32)(n - 1)) == 0)
        return 1;
    if (cm->arch == ARCH_V6M || must || (ccr & 8)) {
        fault_raise(EXC_USAGE, UFSR_UNALIGNED);
        return 0;
    }
    return 1;
}

static u32 mem_ld(u32 a, int n, int must)
{
    return aligned_ok(a, n, must) ? ld(a, n) : 0;
}

static void mem_st(u32 a, int n, u32 v, int must)
{
    if (aligned_ok(a, n, must))
        st(a, n, v);
}

/* ---- registers, flags and the program counter ------------------------ */

static u32 reg(int n)
{
    return n == 15 ? pc + 4 : R[n];
}

static void set_sp(u32 v)
{
    R[13] = v & ~3u;
}

static void branch_to(u32 a)            /* BranchWritePC */
{
    npc = a & ~1u;
    pc_written = 1;
}

static void bx_to(u32 a)                /* BXWritePC, LoadWritePC */
{
    if (ipsr && (a >> 28) == 0xF) {
        exc_return_pending = 1;
        exc_return_value = a;
        pc_written = 1;
        return;
    }
    tbit = (int)(a & 1);
    npc = a & ~1u;
    pc_written = 1;
}

static void set_reg(int n, u32 v)       /* with ALUWritePC */
{
    if (n == 15)
        branch_to(v);
    else if (n == 13)
        set_sp(v);
    else
        R[n] = v;
}

static void nz(u32 r)
{
    N = (int)(r >> 31);
    Z = r == 0;
}

static u32 add_c(u32 x, u32 y, int cin, int *c, int *v)
{
    u64 us = (u64)x + y + (u32)cin;
    u32 r = (u32)us;
    *c = (int)(us >> 32);
    *v = (int)((~(x ^ y) & (x ^ r)) >> 31);
    return r;
}

enum { SH_LSL, SH_LSR, SH_ASR, SH_ROR, SH_RRX };

static u32 ror32(u32 v, u32 n)
{
    n &= 31;
    return n ? v >> n | v << (32 - n) : v;
}

static u32 shift_c(u32 v, int t, u32 n, int cin, int *cout)
{
    *cout = cin;
    if (t == SH_RRX) {
        *cout = (int)(v & 1);
        return v >> 1 | (u32)cin << 31;
    }
    if (n == 0)
        return v;
    switch (t) {
    case SH_LSL:
        if (n < 32) {
            *cout = (int)(v >> (32 - n) & 1);
            return v << n;
        }
        *cout = n == 32 ? (int)(v & 1) : 0;
        return 0;
    case SH_LSR:
        if (n < 32) {
            *cout = (int)(v >> (n - 1) & 1);
            return v >> n;
        }
        *cout = n == 32 ? (int)(v >> 31) : 0;
        return 0;
    case SH_ASR:
        if (n < 32) {
            *cout = (int)(v >> (n - 1) & 1);
            return (u32)((s32)v >> n);
        }
        *cout = (int)(v >> 31);
        return (u32)((s32)v >> 31);
    default: {
        u32 r = ror32(v, n);
        *cout = (int)(r >> 31);
        return r;
    }
    }
}

static u32 shift(u32 v, int t, u32 n, int cin)
{
    int c;
    return shift_c(v, t, n, cin, &c);
}

/* DecodeImmShift */
static void imm_shift(int type, u32 imm5, int *t, u32 *n)
{
    *t = type;
    *n = imm5;
    if ((type == SH_LSR || type == SH_ASR) && imm5 == 0)
        *n = 32;
    if (type == SH_ROR && imm5 == 0) {
        *t = SH_RRX;
        *n = 1;
    }
}

static u32 expand_imm_c(u32 imm12, int cin, int *cout)
{
    *cout = cin;
    if ((imm12 >> 10) == 0) {
        u32 b = imm12 & 0xff;
        switch ((imm12 >> 8) & 3) {
        case 0: return b;
        case 1: return b | b << 16;
        case 2: return b << 8 | b << 24;
        default: return b * 0x01010101u;
        }
    }
    u32 r = ror32(0x80 | (imm12 & 0x7f), imm12 >> 7);
    *cout = (int)(r >> 31);
    return r;
}

static int cond_passed(u32 cond)
{
    int r;
    switch (cond >> 1) {
    case 0: r = Z; break;
    case 1: r = C; break;
    case 2: r = N; break;
    case 3: r = V; break;
    case 4: r = C && !Z; break;
    case 5: r = N == V; break;
    case 6: r = N == V && !Z; break;
    default: r = 1; break;
    }
    if ((cond & 1) && cond != 15)
        r = !r;
    return r;
}

static int in_it(void)
{
    return (itstate & 0xf) != 0;
}

static void it_advance(void)
{
    if ((itstate & 7) == 0)
        itstate = 0;
    else
        itstate = (itstate & 0xe0) | ((itstate << 1) & 0x1f);
}

static u32 xpsr(void)
{
    u32 v = (u32)N << 31 | (u32)Z << 30 | (u32)C << 29 | (u32)V << 28 |
            (u32)Q << 27 | (u32)tbit << 24 | (ipsr & 0x1ff);
    if (cm->dsp)
        v |= GE << 16;
    v |= (itstate & 3) << 25 | (itstate >> 2) << 10;
    return v;
}

static void set_apsr(u32 v)
{
    N = (int)(v >> 31 & 1);
    Z = (int)(v >> 30 & 1);
    C = (int)(v >> 29 & 1);
    V = (int)(v >> 28 & 1);
    Q = (int)(v >> 27 & 1);
}

static int privileged(void)
{
    return ipsr != 0 || !(control & 1);
}

/* switch the active stack, keeping the other banked */
static void use_psp(int psp)
{
    if (psp == psp_active)
        return;
    u32 t = R[13];
    R[13] = other_sp;
    other_sp = t;
    psp_active = psp;
}

static u32 get_msp(void)
{
    return psp_active ? other_sp : R[13];
}

static u32 get_psp(void)
{
    return psp_active ? R[13] : other_sp;
}

static void set_msp(u32 v)
{
    if (psp_active)
        other_sp = v & ~3u;
    else
        R[13] = v & ~3u;
}

static void set_psp(u32 v)
{
    if (psp_active)
        R[13] = v & ~3u;
    else
        other_sp = v & ~3u;
}

/* ---- exceptions ------------------------------------------------------ */

static void lockup(const char *why)
{
    state = END_LOCKUP;
    snprintf(end_why, sizeof end_why, "lockup: %s at 0x%08x", why, pc);
}

static int fp_enabled(void);

/* ExceptionEntry: push the frame onto the stack in use, and go to the
 * handler. `ret` is the address the frame returns to. */
static void exc_entry(int n, u32 ret)
{
    int fp_frame = cm->fpu && (control & 4);
    u32 frame = fp_frame ? 0x68 : 0x20;
    /* PushStack: with CCR.STKALIGN (always on ARMv6-M) the frame is
     * 8-aligned, and xPSR bit 9 records that 4 bytes were skipped */
    int force = cm->arch == ARCH_V6M || (ccr & (1u << 9));
    u32 align = force ? (R[13] >> 2 & 1) : 0;
    u32 sp = (R[13] - frame) & ~(force ? 4u : 0u);
    u32 x = xpsr() & ~(1u << 9);
    if (align)
        x |= 1u << 9;
    st(sp, 4, R[0]);
    st(sp + 4, 4, R[1]);
    st(sp + 8, 4, R[2]);
    st(sp + 12, 4, R[3]);
    st(sp + 16, 4, R[12]);
    st(sp + 20, 4, R[14]);
    st(sp + 24, 4, ret);
    st(sp + 28, 4, x);
    if (fp_frame) {
        for (int i = 0; i < 16; i++)
            st(sp + 32 + 4 * (u32)i, 4, S[i]);
        st(sp + 96, 4, fpscr);
    }
    if (fault_exc) {
        /* a stacking fault: give up on the exception rather than model
         * the derived fault's own stacking */
        fault_exc = 0;
        lockup("a fault while stacking an exception frame");
        return;
    }
    R[13] = sp;
    u32 lr;
    if (ipsr)
        lr = 0xFFFFFFF1u;
    else
        lr = psp_active ? 0xFFFFFFFDu : 0xFFFFFFF9u;
    if (fp_frame)
        lr &= ~0x10u;
    R[14] = lr;
    use_psp(0);
    ipsr = (u32)n;
    active[n] = 1;
    clr_pend(n);
    itstate = 0;
    excl_valid = 0;
    if (cm->fpu)
        control &= ~4u;
    u32 v = ld(vtor + 4 * (u32)n, 4);
    if (fault_exc) {
        fault_exc = 0;
        lockup("the vector table cannot be read");
        return;
    }
    if (!(v & 1)) {
        /* a vector without the Thumb bit: the first instruction would
         * take an INVSTATE UsageFault */
        tbit = 0;
    } else {
        tbit = 1;
    }
    pc = v & ~1u;
}

/* A synchronous fault, or SVC: taken at once, escalated to HardFault when
 * its own handler is disabled or cannot preempt, and a lockup when even
 * HardFault cannot. */
static void take_sync(int n, u32 bits, u32 ret)
{
    int ep = exec_prio();
    if (n != EXC_HARD) {
        int enabled = 1;
        if (n == EXC_MEM)
            enabled = (shcsr >> 16) & 1;
        else if (n == EXC_BUS)
            enabled = (shcsr >> 17) & 1;
        else if (n == EXC_USAGE)
            enabled = (shcsr >> 18) & 1;
        if (n != EXC_SVC)
            cfsr |= bits;
        if (!enabled || group_prio(n) >= ep) {
            if (cm->arch == ARCH_V7M)
                hfsr |= 1u << 30;           /* FORCED */
            n = EXC_HARD;
        }
    }
    if (n == EXC_HARD && ep <= -1) {
        lockup("a fault with HardFault already active");
        return;
    }
    exc_entry(n, ret);
}

/* the most urgent pending exception that can preempt now, or 0 */
static int pending_to_take(void)
{
    int best = 0, bp = 0;
    for (int n = 2; n < NEXC; n++) {
        if (!pend[n] || (n >= 16 && !irq_en[n]))
            continue;
        int p = group_prio(n);
        if (!best || p < bp) {
            best = n;
            bp = p;
        }
    }
    if (best && bp < exec_prio())
        return best;
    return 0;
}

static void exc_return(u32 ret)
{
    int n = (int)ipsr;
    active[n] = 0;
    if (n == EXC_HARD || n == EXC_NMI)
        faultmask = 0;
    int to_thread = (ret & 8) != 0;
    int psp = (ret & 4) != 0;
    int fp_frame = cm->fpu && !(ret & 0x10);
    if (!to_thread && psp) {
        take_sync(EXC_USAGE, UFSR_INVPC, pc);
        return;
    }
    use_psp(to_thread && psp);
    if (to_thread)
        control = (control & ~2u) | (psp ? 2u : 0u);
    u32 sp = R[13];
    R[0] = ld(sp, 4);
    R[1] = ld(sp + 4, 4);
    R[2] = ld(sp + 8, 4);
    R[3] = ld(sp + 12, 4);
    R[12] = ld(sp + 16, 4);
    R[14] = ld(sp + 20, 4);
    u32 ra = ld(sp + 24, 4);
    u32 x = ld(sp + 28, 4);
    u32 frame = 0x20;
    if (fp_frame) {
        for (int i = 0; i < 16; i++)
            S[i] = ld(sp + 32 + 4 * (u32)i, 4);
        fpscr = ld(sp + 96, 4);
        frame = 0x68;
    }
    if (fault_exc) {
        fault_exc = 0;
        lockup("a fault while unstacking an exception frame");
        return;
    }
    R[13] = (sp + frame) | ((x >> 9 & 1) ? 4 : 0);
    if (cm->fpu)
        control = (control & ~4u) | (fp_frame ? 4u : 0u);
    set_apsr(x);
    if (cm->dsp)
        GE = x >> 16 & 15;
    itstate = (x >> 25 & 3) | (x >> 8 & 0xfc);
    tbit = (int)(x >> 24 & 1);
    ipsr = to_thread ? 0 : (x & 0x1ff);
    excl_valid = 0;
    pc = ra & ~1u;
}

static void systick_advance(u32 c)
{
    if (!(st_csr & 1))
        return;
    while (c--) {
        if (st_cvr == 0) {
            st_cvr = st_rvr;
            continue;
        }
        if (--st_cvr == 0) {
            st_csr |= 1u << 16;
            if (st_csr & 2)
                set_pend(EXC_SYSTICK);
        }
    }
}

/* ---- floating point -------------------------------------------------- */

static int fp_enabled(void)
{
    if (!cm->fpu)
        return 0;
    u32 a = privileged() ? 1 : 2;
    return ((cpacr >> 20) & 3) >= a && ((cpacr >> 22) & 3) >= a;
}

static float f_of(u32 b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static u32 b_of(float f)
{
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

static double d_of(u64 b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static u64 bd_of(double d)
{
    u64 b;
    memcpy(&b, &d, 8);
    return b;
}

static u64 getd(int n)
{
    return (u64)S[2 * n + 1] << 32 | S[2 * n];
}

static void setd(int n, u64 v)
{
    S[2 * n] = (u32)v;
    S[2 * n + 1] = (u32)(v >> 32);
}

/* The architecture's NaN rules, which the host's arithmetic does not
 * promise: a signalling NaN operand wins over a quiet one, the first
 * operand over the second, and the result is the operand quietened (or
 * the default NaN with FPSCR.DN). */
static int nan_s(u32 a)
{
    return (a & 0x7f800000u) == 0x7f800000u && (a & 0x7fffffu);
}

static int snan_s(u32 a)
{
    return nan_s(a) && !(a & 0x400000u);
}

static int nan_d(u64 a)
{
    return (a & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
           (a & 0xfffffffffffffull);
}

static int snan_d(u64 a)
{
    return nan_d(a) && !(a & 0x8000000000000ull);
}

static u32 qnan_s(u32 a)
{
    return (fpscr & (1u << 25)) ? 0x7fc00000u : a | 0x400000u;
}

static u64 qnan_d(u64 a)
{
    return (fpscr & (1u << 25)) ? 0x7ff8000000000000ull
                                : a | 0x8000000000000ull;
}

static int pick_nan_s(const u32 *ops, int n, u32 *r)
{
    for (int i = 0; i < n; i++)
        if (snan_s(ops[i])) {
            *r = qnan_s(ops[i]);
            return 1;
        }
    for (int i = 0; i < n; i++)
        if (nan_s(ops[i])) {
            *r = qnan_s(ops[i]);
            return 1;
        }
    return 0;
}

static int pick_nan_d(const u64 *ops, int n, u64 *r)
{
    for (int i = 0; i < n; i++)
        if (snan_d(ops[i])) {
            *r = qnan_d(ops[i]);
            return 1;
        }
    for (int i = 0; i < n; i++)
        if (nan_d(ops[i])) {
            *r = qnan_d(ops[i]);
            return 1;
        }
    return 0;
}

/* FPSCR.FZ: denormal inputs and outputs are zero */
static u32 fz_s(u32 a)
{
    if ((fpscr & (1u << 24)) && (a & 0x7f800000u) == 0 && (a & 0x7fffffu))
        return a & 0x80000000u;
    return a;
}

static u64 fz_d(u64 a)
{
    if ((fpscr & (1u << 24)) && (a & 0x7ff0000000000000ull) == 0 &&
        (a & 0xfffffffffffffull))
        return a & 0x8000000000000000ull;
    return a;
}

/* a result that is NaN from valid operands is the default NaN */
static u32 res_s(float f)
{
    u32 r = b_of(f);
    if (nan_s(r))
        return 0x7fc00000u;
    return fz_s(r);
}

static u64 res_d(double d)
{
    u64 r = bd_of(d);
    if (nan_d(r))
        return 0x7ff8000000000000ull;
    return fz_d(r);
}

enum { FOP_ADD, FOP_SUB, FOP_MUL, FOP_DIV, FOP_FMA };

static u32 fop_s(int op, u32 a, u32 b, u32 c)
{
    u32 ops[3], r;
    a = fz_s(a);
    b = fz_s(b);
    c = fz_s(c);
    if (op == FOP_FMA) {
        ops[0] = c;             /* the addend first: FPMulAdd's order */
        ops[1] = a;
        ops[2] = b;
        if (pick_nan_s(ops, 3, &r))
            return r;
        return res_s(fmaf(f_of(a), f_of(b), f_of(c)));
    }
    ops[0] = a;
    ops[1] = b;
    if (pick_nan_s(ops, 2, &r))
        return r;
    float x = f_of(a), y = f_of(b);
    switch (op) {
    case FOP_ADD: return res_s(x + y);
    case FOP_SUB: return res_s(x - y);
    case FOP_MUL: return res_s(x * y);
    default: return res_s(x / y);
    }
}

static u64 fop_d(int op, u64 a, u64 b, u64 c)
{
    u64 ops[3], r;
    a = fz_d(a);
    b = fz_d(b);
    c = fz_d(c);
    if (op == FOP_FMA) {
        ops[0] = c;
        ops[1] = a;
        ops[2] = b;
        if (pick_nan_d(ops, 3, &r))
            return r;
        return res_d(fma(d_of(a), d_of(b), d_of(c)));
    }
    ops[0] = a;
    ops[1] = b;
    if (pick_nan_d(ops, 2, &r))
        return r;
    double x = d_of(a), y = d_of(b);
    switch (op) {
    case FOP_ADD: return res_d(x + y);
    case FOP_SUB: return res_d(x - y);
    case FOP_MUL: return res_d(x * y);
    default: return res_d(x / y);
    }
}

static void fcmp_flags(int unord, int eq, int lt)
{
    u32 f = unord ? 0x3u : eq ? 0x6u : lt ? 0x8u : 0x2u;
    fpscr = (fpscr & 0x0fffffffu) | f << 28;
}

/* round a double to an integer: toward zero, or as FPSCR.RMode says */
static double round_by(double x, int mode)
{
    double t = floor(x), d = x - t;
    switch (mode) {
    case 0:                             /* nearest, ties to even */
        if (d > 0.5 || (d == 0.5 && fmod(t, 2.0) != 0.0))
            t += 1.0;
        return t;
    case 1: return ceil(x);             /* toward +inf */
    case 2: return floor(x);            /* toward -inf */
    default: return x < 0 ? ceil(x) : floor(x);
    }
}

/* VCVT to a 32-bit integer: NaN is 0, out of range saturates */
static u32 to_int(double x, int is_signed, int mode)
{
    if (x != x)
        return 0;
    double r = round_by(x, mode);
    if (is_signed) {
        if (r >= 2147483648.0)
            return 0x7fffffffu;
        if (r < -2147483648.0)
            return 0x80000000u;
        return (u32)(s32)r;
    }
    if (r >= 4294967296.0)
        return 0xffffffffu;
    if (r < 0)
        return 0;
    return (u32)r;
}

static u32 vfp_imm_s(u32 imm8)
{
    return (imm8 & 0x80) << 24 | ((imm8 & 0x40) ? 0x3E000000u : 0x40000000u) |
           (imm8 & 0x3f) << 19;
}

static u64 vfp_imm_d(u32 imm8)
{
    u32 hi = (imm8 & 0x80) << 24 |
             ((imm8 & 0x40) ? 0x3FC00000u : 0x40000000u) | (imm8 & 0x3f) << 16;
    return (u64)hi << 32;
}

static void undef(void)
{
    fault_raise(EXC_USAGE, UFSR_UNDEFINSTR);
}

/* the FP context becomes active with the first FP instruction */
static int fp_begin(void)
{
    if (!fp_enabled()) {
        fault_raise(EXC_USAGE, UFSR_NOCP);
        return 0;
    }
    if (fpccr & 0x80000000u)            /* ASPEN */
        control |= 4;
    return 1;
}

/* Extension register load and store, and the 64-bit moves. */
static void vfp_ldst(u32 h1, u32 h2)
{
    int P = (int)(h1 >> 8 & 1), U = (int)(h1 >> 7 & 1);
    int D = (int)(h1 >> 6 & 1), W = (int)(h1 >> 5 & 1);
    int L = (int)(h1 >> 4 & 1), rn = (int)(h1 & 15);
    u32 vd = h2 >> 12 & 15, imm8 = h2 & 0xff;
    int dbl = (int)(h2 >> 8 & 1);
    if (!fp_begin())
        return;
    if (!P && !U && D && !W) {          /* VMOV two core registers */
        int rt = (int)(h2 >> 12 & 15), rt2 = rn;
        u32 m = (h2 & 15), M = h2 >> 5 & 1;
        if (dbl) {
            int dm = (int)(M << 4 | m);
            if (dm >= 16 && cm->fpu != FPU_DP) {
                undef();
                return;
            }
            if (L) {
                set_reg(rt, S[2 * dm]);
                set_reg(rt2, S[2 * dm + 1]);
            } else {
                S[2 * dm] = R[rt];
                S[2 * dm + 1] = R[rt2];
            }
        } else {
            int sm = (int)(m << 1 | M);
            if (L) {
                set_reg(rt, S[sm]);
                set_reg(rt2, S[(sm + 1) & 31]);
            } else {
                S[sm] = R[rt];
                S[(sm + 1) & 31] = R[rt2];
            }
        }
        return;
    }
    if (!P && !U) {
        undef();
        return;
    }
    u32 base = rn == 15 ? (pc + 4) & ~3u : R[rn];
    if (P && !W) {                      /* VLDR, VSTR */
        u32 a = U ? base + imm8 * 4 : base - imm8 * 4;
        if (dbl) {
            int d = D << 4 | (int)vd;
            if (L) {
                u32 lo = mem_ld(a, 4, 1), hi = mem_ld(a + 4, 4, 1);
                S[2 * d] = lo;
                S[2 * d + 1] = hi;
            } else {
                mem_st(a, 4, S[2 * d], 1);
                mem_st(a + 4, 4, S[2 * d + 1], 1);
            }
        } else {
            int d = (int)(vd << 1) | D;
            if (L)
                S[d] = mem_ld(a, 4, 1);
            else
                mem_st(a, 4, S[d], 1);
        }
        return;
    }
    if (P == U && W) {                  /* PU=11 with W: undefined */
        undef();
        return;
    }
    /* VLDM, VSTM, VPUSH, VPOP */
    u32 words = dbl ? (imm8 & ~1u) : imm8;
    u32 a = U ? base : base - imm8 * 4;
    int first = dbl ? (D << 4 | (int)vd) * 2 : (int)(vd << 1) | D;
    for (u32 i = 0; i < words; i++) {
        int s = first + (int)i;
        if (s >= 64)
            break;
        if (L)
            S[s] = mem_ld(a + 4 * i, 4, 1);
        else
            mem_st(a + 4 * i, 4, S[s], 1);
    }
    if (W && !fault_exc)
        set_reg(rn, U ? base + imm8 * 4 : base - imm8 * 4);
}

/* 8, 16 and 32-bit transfers between the core and the FPU */
static void vfp_xfer(u32 h1, u32 h2)
{
    u32 A = h1 >> 5 & 7, L = h1 >> 4 & 1, Cb = h2 >> 8 & 1;
    int rt = (int)(h2 >> 12 & 15);
    if (!fp_begin())
        return;
    if (!Cb && A == 0) {                /* VMOV core <-> single */
        int n = (int)((h1 & 15) << 1 | (h2 >> 7 & 1));
        if (L)
            set_reg(rt, S[n]);
        else
            S[n] = R[rt];
        return;
    }
    if (!Cb && A == 7) {                /* VMRS, VMSR */
        if ((h1 & 15) != 1) {
            undef();
            return;
        }
        if (L) {
            if (rt == 15) {
                N = (int)(fpscr >> 31);
                Z = (int)(fpscr >> 30 & 1);
                C = (int)(fpscr >> 29 & 1);
                V = (int)(fpscr >> 28 & 1);
            } else
                set_reg(rt, fpscr);
        } else
            fpscr = R[rt] & 0xF7C0009Fu;
        return;
    }
    if (Cb && (h1 & 0xc0) == 0 && (h2 >> 5 & 3) == 0) {
        /* VMOV between a core register and one half of a D register:
         * the register is D:Vd (or N:Vn), D in the second halfword */
        int d = (int)((h2 >> 7 & 1) << 4 | (h1 & 15));
        int x = (int)(h1 >> 5 & 1);
        if (d >= 16) {
            undef();
            return;
        }
        if (L)
            set_reg(rt, S[2 * d + x]);
        else
            S[2 * d + x] = R[rt];
        return;
    }
    undef();
}

static void vfp_dp(u32 h1, u32 h2)
{
    u32 opc1 = (h1 >> 4 & 0xb);
    int D = (int)(h1 >> 6 & 1), Nb = (int)(h2 >> 7 & 1);
    int M = (int)(h2 >> 5 & 1), op = (int)(h2 >> 6 & 1);
    u32 vn = h1 & 15, vd = h2 >> 12 & 15, vm = h2 & 15;
    int dbl = (int)(h2 >> 8 & 1);
    if (!fp_begin())
        return;
    if (dbl && cm->fpu != FPU_DP) {
        undef();
        return;
    }
    int d = dbl ? D << 4 | (int)vd : (int)(vd << 1) | D;
    int n = dbl ? Nb << 4 | (int)vn : (int)(vn << 1) | Nb;
    int m = dbl ? M << 4 | (int)vm : (int)(vm << 1) | M;
    if (opc1 != 0xb) {
        if (dbl && (d >= 16 || n >= 16 || m >= 16)) {
            undef();
            return;
        }
        /* the arithmetic: dest, n and m */
        if (dbl) {
            u64 a = getd(n), b = getd(m), acc = getd(d), r;
            switch (opc1) {
            case 0x0: case 0x1: {           /* VMLA VMLS, VNMLS VNMLA */
                u64 p = fop_d(FOP_MUL, a, b, 0);
                if (op)
                    p ^= 0x8000000000000000ull;
                if (opc1 == 1)
                    acc ^= 0x8000000000000000ull;
                r = fop_d(FOP_ADD, acc, p, 0);
                break;
            }
            case 0x2:                       /* VMUL, VNMUL */
                r = fop_d(FOP_MUL, a, b, 0);
                if (op)
                    r ^= 0x8000000000000000ull;
                break;
            case 0x3:
                r = fop_d(op ? FOP_SUB : FOP_ADD, a, b, 0);
                break;
            case 0x8:
                if (op) { undef(); return; }
                r = fop_d(FOP_DIV, a, b, 0);
                break;
            case 0x9: case 0xa:             /* VFNMA VFNMS, VFMA VFMS */
                if (op)
                    a ^= 0x8000000000000000ull;
                if (opc1 == 9)
                    acc ^= 0x8000000000000000ull;
                r = fop_d(FOP_FMA, a, b, acc);
                break;
            default:
                undef();
                return;
            }
            setd(d, r);
        } else {
            u32 a = S[n], b = S[m], acc = S[d], r;
            switch (opc1) {
            case 0x0: case 0x1: {
                u32 p = fop_s(FOP_MUL, a, b, 0);
                if (op)
                    p ^= 0x80000000u;
                if (opc1 == 1)
                    acc ^= 0x80000000u;
                r = fop_s(FOP_ADD, acc, p, 0);
                break;
            }
            case 0x2:
                r = fop_s(FOP_MUL, a, b, 0);
                if (op)
                    r ^= 0x80000000u;
                break;
            case 0x3:
                r = fop_s(op ? FOP_SUB : FOP_ADD, a, b, 0);
                break;
            case 0x8:
                if (op) { undef(); return; }
                r = fop_s(FOP_DIV, a, b, 0);
                break;
            case 0x9: case 0xa:
                if (op)
                    a ^= 0x80000000u;
                if (opc1 == 9)
                    acc ^= 0x80000000u;
                r = fop_s(FOP_FMA, a, b, acc);
                break;
            default:
                undef();
                return;
            }
            S[d] = r;
        }
        return;
    }
    /* the other operations: opc2 is the Vn field */
    u32 opc2 = vn;
    int opc3 = (int)(h2 >> 6 & 3);
    /* which operands are double: the destination, except where a
     * conversion writes a single; the source, except where it reads one */
    int dd = dbl && opc2 != 0x7 && opc2 != 0xc && opc2 != 0xd;
    int dm = dbl && opc2 != 0x8;
    if ((dd && d >= 16) || (dm && m >= 16)) {
        undef();
        return;
    }
    if (!(opc3 & 1)) {                      /* VMOV immediate */
        u32 imm8 = (h1 & 15) << 4 | (h2 & 15);
        if (dbl)
            setd(d, vfp_imm_d(imm8));
        else
            S[d] = vfp_imm_s(imm8);
        return;
    }
    switch (opc2) {
    case 0x0:                               /* VMOV reg, VABS */
        if (dbl)
            setd(d, opc3 == 1 ? getd(m) : getd(m) & ~0x8000000000000000ull);
        else
            S[d] = opc3 == 1 ? S[m] : S[m] & ~0x80000000u;
        return;
    case 0x1:                               /* VNEG, VSQRT */
        if (opc3 == 1) {
            if (dbl)
                setd(d, getd(m) ^ 0x8000000000000000ull);
            else
                S[d] = S[m] ^ 0x80000000u;
            return;
        }
        if (dbl) {
            u64 a = fz_d(getd(m)), r;
            if (pick_nan_d(&a, 1, &r))
                setd(d, r);
            else
                setd(d, res_d(sqrt(d_of(a))));
        } else {
            u32 a = fz_s(S[m]), r;
            if (pick_nan_s(&a, 1, &r))
                S[d] = r;
            else
                S[d] = res_s(sqrtf(f_of(a)));
        }
        return;
    case 0x4: case 0x5: {                   /* VCMP, VCMPE */
        if (dbl) {
            u64 a = fz_d(getd(d)), b = opc2 == 5 ? 0 : fz_d(getd(m));
            double x = d_of(a), y = d_of(b);
            fcmp_flags(x != x || y != y, x == y, x < y);
        } else {
            u32 a = fz_s(S[d]), b = opc2 == 5 ? 0 : fz_s(S[m]);
            float x = f_of(a), y = f_of(b);
            fcmp_flags(x != x || y != y, x == y, x < y);
        }
        return;
    }
    case 0x7:                               /* VCVT between precisions */
        if (opc3 != 3 || cm->fpu != FPU_DP) {
            undef();
            return;
        }
        if (dbl) {                          /* double -> single */
            int sd = (int)(vd << 1) | D;
            (void)dd;
            u64 a = fz_d(getd(m)), r;
            if (pick_nan_d(&a, 1, &r)) {
                u32 hi = (u32)(r >> 32);
                S[sd] = (fpscr & (1u << 25)) ? 0x7fc00000u
                        : (hi & 0x80000000u) | 0x7fc00000u |
                          (u32)(r >> 29 & 0x3fffff);
            } else
                S[sd] = res_s((float)d_of(a));
        } else {                            /* single -> double */
            int dd2 = D << 4 | (int)vd;
            int sm = (int)(vm << 1) | M;
            if (dd2 >= 16) {
                undef();
                return;
            }
            u32 a = fz_s(S[sm]);
            if (nan_s(a)) {
                u32 q = qnan_s(a);
                setd(dd2, (fpscr & (1u << 25)) ? 0x7ff8000000000000ull
                         : (u64)(q & 0x80000000u) << 32 |
                           0x7ff8000000000000ull |
                           (u64)(q & 0x3fffffu) << 29);
            } else
                setd(dd2, res_d((double)f_of(a)));
        }
        return;
    case 0x8: {                             /* VCVT integer -> float */
        int sm = (int)(vm << 1) | M;
        int is_signed = opc3 >> 1;
        double x = is_signed ? (double)(s32)S[sm] : (double)S[sm];
        if (dbl)
            setd(d, res_d(x));
        else
            S[d] = res_s((float)x);
        return;
    }
    case 0xc: case 0xd: {                   /* VCVT, VCVTR float -> int */
        int sd = (int)(vd << 1) | D;
        int mode = (opc3 & 2) ? 3 : (int)(fpscr >> 22 & 3);
        double x = dbl ? d_of(fz_d(getd(m))) : (double)f_of(fz_s(S[m]));
        S[sd] = to_int(x, opc2 == 0xd, mode);
        return;
    }
    }
    undef();
}

/* ---- semihosting ----------------------------------------------------- */

static void semihost(void)
{
    u32 op = R[0], arg = R[1];
    switch (op) {
    case 0x03:                          /* SYS_WRITEC */
        out_byte((int)ld(arg, 1));
        return;
    case 0x04:                          /* SYS_WRITE0 */
        for (u32 a = arg;; a++) {
            u32 c = ld(a, 1);
            if (!c || fault_exc)
                break;
            out_byte((int)c);
        }
        return;
    case 0x05: {                        /* SYS_WRITE */
        u32 fh = ld(arg, 4), buf = ld(arg + 4, 4), len = ld(arg + 8, 4);
        for (u32 i = 0; i < len && !fault_exc; i++) {
            int c = (int)ld(buf + i, 1);
            if (fh == 2)
                fputc(c, stderr);
            else
                out_byte(c);
        }
        R[0] = 0;
        return;
    }
    case 0x01:                          /* SYS_OPEN: only ":tt" */
        R[0] = 1;
        return;
    case 0x02:                          /* SYS_CLOSE */
    case 0x09:                          /* SYS_ISTTY */
        R[0] = op == 9;
        return;
    case 0x06:                          /* SYS_READ: nothing to read */
        R[0] = ld(arg + 8, 4);
        return;
    case 0x07:
        R[0] = (u32)getchar();
        return;
    case 0x0c:
        R[0] = 0;
        return;
    case 0x10:                          /* SYS_CLOCK: centiseconds */
        R[0] = (u32)(cycles / 250000);  /* at the MPS2's 25 MHz */
        return;
    case 0x11:
        R[0] = 0;
        return;
    case 0x13:
        R[0] = 0;
        return;
    case 0x15:                          /* SYS_GET_CMDLINE: empty */
        st(ld(arg, 4), 1, 0);
        st(arg + 4, 4, 0);
        R[0] = 0;
        return;
    case 0x16: {                        /* SYS_HEAPINFO: unknown */
        u32 blk = ld(arg, 4);
        for (int i = 0; i < 4; i++)
            st(blk + 4 * (u32)i, 4, 0);
        R[0] = 0;
        return;
    }
    case 0x18:                          /* SYS_EXIT */
        state = END_EXIT;
        exit_status = arg == 0x20026 ? 0 : 1;
        snprintf(end_why, sizeof end_why, "SYS_EXIT (0x%x)", arg);
        return;
    case 0x20:                          /* SYS_EXIT_EXTENDED */
        state = END_EXIT;
        exit_status = ld(arg, 4) == 0x20026 ? (int)ld(arg + 4, 4) : 1;
        snprintf(end_why, sizeof end_why, "SYS_EXIT_EXTENDED (%d)",
                 exit_status);
        return;
    }
    R[0] = (u32)-1;
}

/* ---- 16-bit instructions --------------------------------------------- */

static int wfx_idle(void);

static void ldst_mult(int rn, u32 list, int load, int wb, int db)
{
    int count = 0;
    for (int i = 0; i < 16; i++)
        count += (int)(list >> i & 1);
    u32 base = R[rn];
    u32 a = db ? base - 4 * (u32)count : base;
    u32 vals[16];
    for (int i = 0; i < 16; i++) {
        if (!(list >> i & 1))
            continue;
        if (load)
            vals[i] = mem_ld(a, 4, 1);
        else
            mem_st(a, 4, reg(i), 1);
        a += 4;
    }
    if (fault_exc)
        return;
    if (wb)
        set_reg(rn, db ? base - 4 * (u32)count : base + 4 * (u32)count);
    if (load) {
        for (int i = 0; i < 15; i++)
            if (list >> i & 1)
                set_reg(i, vals[i]);
        if (list >> 15 & 1)
            bx_to(vals[15]);
    }
}

static void exec16(u32 h)
{
    int sf = !in_it();
    int c, v;
    u32 r;
    switch (h >> 12) {
    case 0x0: case 0x1: {
        u32 op = h >> 11 & 3;
        int rd = (int)(h & 7), rm = (int)(h >> 3 & 7);
        if (op != 3) {                  /* LSL, LSR, ASR immediate */
            int t;
            u32 n;
            imm_shift((int)op, h >> 6 & 31, &t, &n);
            r = shift_c(R[rm], t, n, C, &c);
            R[rd] = r;
            if (sf) {
                nz(r);
                C = c;
            }
            return;
        }
        u32 opr = (h & 0x400) ? (h >> 6 & 7) : R[h >> 6 & 7];
        int rn = rm;
        if (h & 0x200)
            r = add_c(R[rn], ~opr, 1, &c, &v);
        else
            r = add_c(R[rn], opr, 0, &c, &v);
        R[rd] = r;
        if (sf) {
            nz(r);
            C = c;
            V = v;
        }
        return;
    }
    case 0x2: case 0x3: {               /* MOV CMP ADD SUB imm8 */
        int rd = (int)(h >> 8 & 7);
        u32 imm = h & 0xff;
        switch (h >> 11 & 3) {
        case 0:
            R[rd] = imm;
            if (sf)
                nz(imm);
            return;
        case 1:
            r = add_c(R[rd], ~imm, 1, &c, &v);
            nz(r);
            C = c;
            V = v;
            return;
        case 2:
            r = add_c(R[rd], imm, 0, &c, &v);
            break;
        default:
            r = add_c(R[rd], ~imm, 1, &c, &v);
            break;
        }
        R[rd] = r;
        if (sf) {
            nz(r);
            C = c;
            V = v;
        }
        return;
    }
    case 0x4:
        if ((h >> 10) == 0x10) {        /* data processing */
            int rdn = (int)(h & 7), rm = (int)(h >> 3 & 7);
            u32 a = R[rdn], b = R[rm];
            int lc = C;
            switch (h >> 6 & 15) {
            case 0x0: r = a & b; break;
            case 0x1: r = a ^ b; break;
            case 0x2: r = shift_c(a, SH_LSL, b & 0xff, C, &lc); break;
            case 0x3: r = shift_c(a, SH_LSR, b & 0xff, C, &lc); break;
            case 0x4: r = shift_c(a, SH_ASR, b & 0xff, C, &lc); break;
            case 0x5:
                r = add_c(a, b, C, &c, &v);
                R[rdn] = r;
                if (sf) { nz(r); C = c; V = v; }
                return;
            case 0x6:
                r = add_c(a, ~b, C, &c, &v);
                R[rdn] = r;
                if (sf) { nz(r); C = c; V = v; }
                return;
            case 0x7: r = shift_c(a, SH_ROR, b & 0xff, C, &lc); break;
            case 0x8:
                nz(a & b);
                return;
            case 0x9:                   /* RSB #0 */
                r = add_c(~b, 0, 1, &c, &v);
                R[rdn] = r;
                if (sf) { nz(r); C = c; V = v; }
                return;
            case 0xa:
                r = add_c(a, ~b, 1, &c, &v);
                nz(r); C = c; V = v;
                return;
            case 0xb:
                r = add_c(a, b, 0, &c, &v);
                nz(r); C = c; V = v;
                return;
            case 0xc: r = a | b; break;
            case 0xd:
                r = a * b;
                R[rdn] = r;
                if (sf)
                    nz(r);
                return;
            case 0xe: r = a & ~b; break;
            default: r = ~b; break;
            }
            R[rdn] = r;
            if (sf) {
                nz(r);
                C = lc;
            }
            return;
        }
        if ((h >> 10) == 0x11) {        /* special data, branch exchange */
            int rdn = (int)((h & 7) | (h >> 4 & 8)), rm = (int)(h >> 3 & 15);
            switch (h >> 8 & 3) {
            case 0:
                set_reg(rdn, reg(rdn) + reg(rm));
                return;
            case 1:
                r = add_c(reg(rdn), ~reg(rm), 1, &c, &v);
                nz(r); C = c; V = v;
                return;
            case 2:
                set_reg(rdn, reg(rm));
                return;
            default:
                if (h & 0x80) {         /* BLX */
                    u32 t = reg(rm);
                    R[14] = (pc + 2) | 1;
                    bx_to(t);
                } else
                    bx_to(reg(rm));
                return;
            }
        }
        /* LDR literal */
        R[h >> 8 & 7] = mem_ld(((pc + 4) & ~3u) + (h & 0xff) * 4, 4, 0);
        return;
    case 0x5: {                         /* load/store register offset */
        int rt = (int)(h & 7);
        u32 a = R[h >> 3 & 7] + R[h >> 6 & 7];
        switch (h >> 9 & 7) {
        case 0: mem_st(a, 4, R[rt], 0); return;
        case 1: mem_st(a, 2, R[rt], 0); return;
        case 2: mem_st(a, 1, R[rt], 0); return;
        case 3: r = (u32)(s32)(int8_t)mem_ld(a, 1, 0); break;
        case 4: r = mem_ld(a, 4, 0); break;
        case 5: r = mem_ld(a, 2, 0); break;
        case 6: r = mem_ld(a, 1, 0); break;
        default: r = (u32)(s32)(int16_t)mem_ld(a, 2, 0); break;
        }
        if (!fault_exc)
            R[rt] = r;
        return;
    }
    case 0x6: case 0x7: {               /* STR LDR STRB LDRB imm5 */
        int rt = (int)(h & 7), byte = (int)(h >> 12 & 1);
        u32 a = R[h >> 3 & 7] + (h >> 6 & 31) * (byte ? 1u : 4u);
        if (h & 0x800) {
            r = mem_ld(a, byte ? 1 : 4, 0);
            if (!fault_exc)
                R[rt] = r;
        } else
            mem_st(a, byte ? 1 : 4, R[rt], 0);
        return;
    }
    case 0x8: {                         /* STRH LDRH imm5 */
        int rt = (int)(h & 7);
        u32 a = R[h >> 3 & 7] + (h >> 6 & 31) * 2;
        if (h & 0x800) {
            r = mem_ld(a, 2, 0);
            if (!fault_exc)
                R[rt] = r;
        } else
            mem_st(a, 2, R[rt], 0);
        return;
    }
    case 0x9: {                         /* STR LDR SP-relative */
        int rt = (int)(h >> 8 & 7);
        u32 a = R[13] + (h & 0xff) * 4;
        if (h & 0x800) {
            r = mem_ld(a, 4, 0);
            if (!fault_exc)
                R[rt] = r;
        } else
            mem_st(a, 4, R[rt], 0);
        return;
    }
    case 0xa:                           /* ADR, ADD SP */
        if (h & 0x800)
            R[h >> 8 & 7] = R[13] + (h & 0xff) * 4;
        else
            R[h >> 8 & 7] = ((pc + 4) & ~3u) + (h & 0xff) * 4;
        return;
    case 0xb:
        if ((h & 0xff00) == 0xb000) {   /* ADD, SUB SP imm7 */
            if (h & 0x80)
                set_sp(R[13] - (h & 0x7f) * 4);
            else
                set_sp(R[13] + (h & 0x7f) * 4);
            return;
        }
        if ((h & 0xf500) == 0xb100) {   /* CBZ, CBNZ */
            if (cm->arch == ARCH_V6M) {
                undef();
                return;
            }
            u32 off = (h >> 3 & 31) << 1 | (h >> 9 & 1) << 6;
            int nzero = R[h & 7] != 0;
            if (nzero == (int)(h >> 11 & 1))
                branch_to(pc + 4 + off);
            return;
        }
        if ((h & 0xff00) == 0xb200) {   /* SXTH SXTB UXTH UXTB */
            u32 m = R[h >> 3 & 7];
            switch (h >> 6 & 3) {
            case 0: r = (u32)(s32)(int16_t)m; break;
            case 1: r = (u32)(s32)(int8_t)m; break;
            case 2: r = m & 0xffff; break;
            default: r = m & 0xff; break;
            }
            R[h & 7] = r;
            return;
        }
        if ((h & 0xfe00) == 0xb400) {   /* PUSH */
            u32 list = (h & 0xff) | (h & 0x100) << 6;
            ldst_mult(13, list, 0, 1, 1);
            return;
        }
        if ((h & 0xfe00) == 0xbc00) {   /* POP */
            u32 list = (h & 0xff) | (h & 0x100) << 7;
            ldst_mult(13, list, 1, 1, 0);
            return;
        }
        if ((h & 0xffe8) == 0xb660) {   /* CPS */
            if (!privileged())
                return;
            if (h & 0x10) {             /* CPSID: mask */
                if (h & 2) primask = 1;
                if ((h & 1) && exec_prio() > -1) faultmask = 1;
            } else {                    /* CPSIE */
                if (h & 2) primask = 0;
                if (h & 1) faultmask = 0;
            }
            return;
        }
        if ((h & 0xff00) == 0xba00) {   /* REV REV16 REVSH */
            u32 m = R[h >> 3 & 7];
            switch (h >> 6 & 3) {
            case 0:
                r = m >> 24 | (m >> 8 & 0xff00) | (m << 8 & 0xff0000) | m << 24;
                break;
            case 1:
                r = (m >> 8 & 0x00ff00ffu) | (m << 8 & 0xff00ff00u);
                break;
            case 3:
                r = (u32)(s32)(int16_t)((m >> 8 & 0xff) | (m & 0xff) << 8);
                break;
            default:
                undef();
                return;
            }
            R[h & 7] = r;
            return;
        }
        if ((h & 0xff00) == 0xbe00) {   /* BKPT */
            if (semihosting && (h & 0xff) == 0xab) {
                semihost();
                return;
            }
            if (cm->arch == ARCH_V7M) {
                hfsr |= 1u << 31;       /* DEBUGEVT */
            }
            fault_raise(EXC_HARD, 0);
            return;
        }
        if ((h & 0xff00) == 0xbf00) {   /* IT, hints */
            if (h & 15) {
                if (cm->arch == ARCH_V6M) {
                    undef();
                    return;
                }
                itstate = h & 0xff;
                return;
            }
            u32 hint = h >> 4 & 15;
            if (hint == 2 || hint == 3)
                wfx_idle();
            return;
        }
        undef();
        return;
    case 0xc: {                         /* STM, LDM */
        int rn = (int)(h >> 8 & 7);
        u32 list = h & 0xff;
        if (h & 0x800)
            ldst_mult(rn, list, 1, !(list >> rn & 1), 0);
        else
            ldst_mult(rn, list, 0, 1, 0);
        return;
    }
    case 0xd: {
        u32 cond = h >> 8 & 15;
        if (cond == 14) {
            undef();
            return;
        }
        if (cond == 15) {               /* SVC */
            fault_raise(EXC_SVC, 0);
            return;
        }
        if (cond_passed(cond))
            branch_to(pc + 4 + (u32)((s32)(int8_t)(h & 0xff) * 2));
        return;
    }
    case 0xe:
        if (!(h & 0x800)) {
            u32 off = (h & 0x7ff) << 1;
            if (off & 0x800)
                off |= 0xfffff000u;
            branch_to(pc + 4 + off);
            return;
        }
        undef();
        return;
    }
    undef();
}

/* ---- 32-bit instructions --------------------------------------------- */

/* the data-processing operations of the modified-immediate and
 * shifted-register forms, which share their opcodes */
static void dp_op(u32 op, int s, int rn, int rd, u32 b, int sc)
{
    u32 a = rn == 15 ? 0 : reg(rn), r;
    int c = C, v = V, logical = 1, test = 0;
    switch (op) {
    case 0x0:                           /* AND, TST */
        r = a & b;
        test = rd == 15;
        break;
    case 0x1: r = a & ~b; break;
    case 0x2: r = rn == 15 ? b : a | b; break;   /* ORR, MOV */
    case 0x3: r = rn == 15 ? ~b : a | ~b; break; /* ORN, MVN */
    case 0x4:                           /* EOR, TEQ */
        r = a ^ b;
        test = rd == 15;
        break;
    case 0x8:                           /* ADD, CMN */
        r = add_c(a, b, 0, &c, &v);
        logical = 0;
        test = rd == 15;
        break;
    case 0xa: r = add_c(a, b, C, &c, &v); logical = 0; break;
    case 0xb: r = add_c(a, ~b, C, &c, &v); logical = 0; break;
    case 0xd:                           /* SUB, CMP */
        r = add_c(a, ~b, 1, &c, &v);
        logical = 0;
        test = rd == 15;
        break;
    case 0xe: r = add_c(b, ~a, 1, &c, &v); logical = 0; break;
    default:
        undef();
        return;
    }
    if (test && !s) {
        undef();
        return;
    }
    if (!test)
        set_reg(rd, r);
    if (s) {
        nz(r);
        if (logical)
            C = sc;
        else {
            C = c;
            V = v;
        }
    }
}

static u32 sat_signed(s64 x, u32 bits, int *sat)
{
    s64 hi = ((s64)1 << (bits - 1)) - 1, lo = -((s64)1 << (bits - 1));
    *sat = 0;
    if (x > hi) { *sat = 1; return (u32)hi; }
    if (x < lo) { *sat = 1; return (u32)lo; }
    return (u32)x;
}

static u32 sat_unsigned(s64 x, u32 bits, int *sat)
{
    s64 hi = bits >= 32 ? (s64)0xffffffffll : ((s64)1 << bits) - 1;
    *sat = 0;
    if (x > hi) { *sat = 1; return (u32)hi; }
    if (x < 0) { *sat = 1; return 0; }
    return (u32)x;
}

static void plain_imm(u32 h1, u32 h2)
{
    u32 op = h1 >> 4 & 0x1f;
    int rn = (int)(h1 & 15), rd = (int)(h2 >> 8 & 15);
    u32 imm12 = (h1 >> 10 & 1) << 11 | (h2 >> 12 & 7) << 8 | (h2 & 0xff);
    u32 imm16 = (h1 & 15) << 12 | imm12;
    u32 lsb = (h2 >> 12 & 7) << 2 | (h2 >> 6 & 3);
    u32 w = h2 & 31;
    int sat;
    switch (op) {
    case 0x00:                          /* ADDW, ADR */
        set_reg(rd, (rn == 15 ? (pc + 4) & ~3u : reg(rn)) + imm12);
        return;
    case 0x0a:                          /* SUBW, ADR */
        set_reg(rd, (rn == 15 ? (pc + 4) & ~3u : reg(rn)) - imm12);
        return;
    case 0x04: set_reg(rd, imm16); return;
    case 0x0c: set_reg(rd, (R[rd] & 0xffff) | imm16 << 16); return;
    case 0x10: case 0x12: {             /* SSAT */
        int t = (h1 >> 5 & 1) ? SH_ASR : SH_LSL;
        u32 n = lsb;
        if (t == SH_ASR && n == 0) {
            undef();                    /* SSAT16 */
            return;
        }
        s32 x = (s32)shift(R[rn], t, n, C);
        R[rd] = sat_signed(x, w + 1, &sat);
        if (sat)
            Q = 1;
        return;
    }
    case 0x18: case 0x1a: {             /* USAT */
        int t = (h1 >> 5 & 1) ? SH_ASR : SH_LSL;
        if (t == SH_ASR && lsb == 0) {
            undef();
            return;
        }
        s32 x = (s32)shift(R[rn], t, lsb, C);
        R[rd] = sat_unsigned(x, w, &sat);
        if (sat)
            Q = 1;
        return;
    }
    case 0x14: {                        /* SBFX */
        u32 x = R[rn] >> lsb;
        u32 width = w + 1;
        if (lsb + width > 32) { undef(); return; }
        R[rd] = width == 32 ? x : (u32)((s32)(x << (32 - width)) >> (32 - width));
        return;
    }
    case 0x1c: {                        /* UBFX */
        u32 width = w + 1;
        if (lsb + width > 32) { undef(); return; }
        R[rd] = width == 32 ? R[rn] >> lsb : (R[rn] >> lsb) & ((1u << width) - 1);
        return;
    }
    case 0x16: {                        /* BFI, BFC */
        u32 msb = w;
        if (msb < lsb) { undef(); return; }
        u32 width = msb - lsb + 1;
        u32 mask = (width == 32 ? 0xffffffffu : ((1u << width) - 1)) << lsb;
        u32 src = rn == 15 ? 0 : R[rn] << lsb;
        R[rd] = (R[rd] & ~mask) | (src & mask);
        return;
    }
    }
    undef();
}

static void msr(u32 h1, u32 h2)
{
    u32 v = R[h1 & 15], sysm = h2 & 0xff, mask = h2 >> 10 & 3;
    if (sysm < 8) {
        if (!(sysm & 4)) {
            if (mask & 2)
                set_apsr(v);
            if ((mask & 1) && cm->dsp)
                GE = v >> 16 & 15;
        }
        return;
    }
    if (!privileged())
        return;
    switch (sysm) {
    case 8: set_msp(v); return;
    case 9: set_psp(v); return;
    case 16: primask = v & 1; return;
    case 17: if (cm->arch == ARCH_V7M) basepri = v & 0xff; return;
    case 18:
        if (cm->arch == ARCH_V7M && (v & 0xff) &&
            ((v & 0xff) < basepri || basepri == 0))
            basepri = v & 0xff;
        return;
    case 19:
        if (cm->arch == ARCH_V7M && exec_prio() > -1)
            faultmask = v & 1;
        return;
    case 20:
        control = (control & ~1u) | (v & 1);
        if (cm->arch == ARCH_V6M)
            control &= ~1u;
        if (!ipsr) {
            control = (control & ~2u) | (v & 2);
            use_psp((control & 2) != 0);
        }
        if (cm->fpu)
            control = (control & ~4u) | (v & 4);
        return;
    }
}

static u32 mrs(u32 h2)
{
    u32 sysm = h2 & 0xff, v = 0;
    if (sysm < 8) {
        if (sysm & 1)
            v |= ipsr & 0x1ff;
        if (!(sysm & 4)) {
            v |= (u32)N << 31 | (u32)Z << 30 | (u32)C << 29 | (u32)V << 28 |
                 (u32)Q << 27;
            if (cm->dsp)
                v |= GE << 16;
        }
        return v;
    }
    if (!privileged() && sysm != 20)
        return 0;
    switch (sysm) {
    case 8: return get_msp();
    case 9: return get_psp();
    case 16: return primask;
    case 17: case 18: return basepri;
    case 19: return faultmask;
    case 20: return control & (cm->fpu ? 7u : 3u);
    }
    return 0;
}

static void branch_misc(u32 h1, u32 h2)
{
    u32 op1 = h2 >> 12 & 5;
    u32 S_ = h1 >> 10 & 1, J1 = h2 >> 13 & 1, J2 = h2 >> 11 & 1;
    if (op1 == 5 || op1 == 1) {         /* BL, B.W */
        if (cm->arch == ARCH_V6M && op1 == 1) {
            undef();
            return;
        }
        u32 I1 = !(J1 ^ S_), I2 = !(J2 ^ S_);
        u32 off = S_ << 24 | I1 << 23 | I2 << 22 | (h1 & 0x3ff) << 12 |
                  (h2 & 0x7ff) << 1;
        if (S_)
            off |= 0xfe000000u;
        if (op1 == 5)
            R[14] = (pc + 4) | 1;
        branch_to(pc + 4 + off);
        return;
    }
    if (op1 == 4) {                     /* BLX imm: no ARM state */
        undef();
        return;
    }
    u32 op = h1 >> 4 & 0x7f;
    if ((op & 0x38) != 0x38) {          /* B<c>.W */
        if (cm->arch == ARCH_V6M) {
            undef();
            return;
        }
        u32 off = S_ << 20 | J2 << 19 | J1 << 18 | (h1 & 0x3f) << 12 |
                  (h2 & 0x7ff) << 1;
        if (S_)
            off |= 0xffe00000u;
        if (cond_passed(h1 >> 6 & 15))
            branch_to(pc + 4 + off);
        return;
    }
    if ((h2 >> 12 & 7) == 2 && op == 0x7f) {
        undef();                        /* UDF.W */
        return;
    }
    if (op1 != 0) {
        undef();
        return;
    }
    switch (op) {
    case 0x38: case 0x39:
        msr(h1, h2);
        return;
    case 0x3a:                          /* hints */
        if (cm->arch == ARCH_V6M) {
            undef();
            return;
        }
        if ((h2 & 0xff) == 2 || (h2 & 0xff) == 3)
            wfx_idle();
        return;
    case 0x3b:                          /* CLREX, DSB, DMB, ISB */
        if ((h2 >> 4 & 15) == 2) {
            if (cm->arch == ARCH_V6M) {
                undef();
                return;
            }
            excl_valid = 0;
        }
        return;
    case 0x3e: case 0x3f:
        set_reg((int)(h2 >> 8 & 15), mrs(h2));
        return;
    }
    undef();
}

/* LDR, LDRB, LDRH, LDRSB, LDRSH, STR, STRB, STRH: every 32-bit form */
static void ldst_single(u32 h1, u32 h2)
{
    int load = (int)(h1 >> 4 & 1), size = (int)(h1 >> 5 & 3);
    int sgn = (int)(h1 >> 8 & 1), rn = (int)(h1 & 15);
    int rt = (int)(h2 >> 12 & 15);
    int n = size == 0 ? 1 : size == 1 ? 2 : 4;
    u32 a, base;
    int wb = 0;
    if (size == 3 || (!load && sgn)) {
        undef();
        return;
    }
    if (rn == 15) {                     /* literal */
        if (!load) {
            undef();
            return;
        }
        u32 imm = h2 & 0xfff;
        base = (pc + 4) & ~3u;
        a = (h1 & 0x80) ? base + imm : base - imm;
    } else if (h1 & 0x80) {             /* imm12 */
        base = R[rn];
        a = base + (h2 & 0xfff);
    } else if ((h2 & 0x800)) {          /* imm8: P U W */
        int P = (int)(h2 >> 10 & 1), U = (int)(h2 >> 9 & 1);
        int W = (int)(h2 >> 8 & 1);
        u32 imm = h2 & 0xff;
        base = R[rn];
        u32 off = U ? base + imm : base - imm;
        a = P ? off : base;
        if (W) {
            wb = 1;
            base = off;
        } else if (!P) {
            undef();
            return;
        }
    } else if ((h2 & 0xfc0) == 0) {     /* register */
        base = R[rn];
        a = base + (R[h2 & 15] << (h2 >> 4 & 3));
    } else {
        undef();
        return;
    }
    if (load && rt == 15 && n != 4)
        return;                         /* PLD, PLI, and other hints */
    if (load) {
        u32 v = mem_ld(a, n, 0);
        if (sgn)
            v = n == 1 ? (u32)(s32)(int8_t)v : (u32)(s32)(int16_t)v;
        if (fault_exc)
            return;
        if (wb)
            set_reg(rn, base);
        if (rt == 15) {
            if (a & 3) {
                fault_raise(EXC_USAGE, UFSR_UNALIGNED);
                return;
            }
            bx_to(v);
        } else
            set_reg(rt, v);
    } else {
        mem_st(a, n, reg(rt), 0);
        if (!fault_exc && wb)
            set_reg(rn, base);
    }
}

static void ldst_dual_excl(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 7 & 3, op2 = h1 >> 4 & 3, op3 = h2 >> 4 & 15;
    int rn = (int)(h1 & 15), rt = (int)(h2 >> 12 & 15);
    int rt2 = (int)(h2 >> 8 & 15), rd = (int)(h2 & 15);
    if ((op1 & 2) || (op2 & 2)) {       /* LDRD, STRD */
        int P = (int)(h1 >> 8 & 1), U = (int)(h1 >> 7 & 1);
        int W = (int)(h1 >> 5 & 1), L = (int)(h1 >> 4 & 1);
        u32 imm = (h2 & 0xff) * 4;
        u32 base = rn == 15 ? (pc + 4) & ~3u : R[rn];
        u32 off = U ? base + imm : base - imm;
        u32 a = P ? off : base;
        if (L) {
            u32 lo = mem_ld(a, 4, 1), hi = mem_ld(a + 4, 4, 1);
            if (fault_exc)
                return;
            if (W)
                set_reg(rn, off);
            set_reg(rt, lo);
            set_reg(rt2, hi);
        } else {
            mem_st(a, 4, R[rt], 1);
            mem_st(a + 4, 4, R[rt2], 1);
            if (!fault_exc && W)
                set_reg(rn, off);
        }
        return;
    }
    if (op1 == 0) {
        u32 a = R[rn] + (h2 & 0xff) * 4;
        if (op2 == 1) {                 /* LDREX */
            u32 v = mem_ld(a, 4, 1);
            if (fault_exc)
                return;
            set_reg(rt, v);
            excl_valid = 1;
            excl_addr = a;
        } else {                        /* STREX: Rd is bits 11:8 */
            int ok = excl_valid && excl_addr == a;
            if (!aligned_ok(a, 4, 1))
                return;
            if (ok)
                st(a, 4, R[rt]);
            if (fault_exc)
                return;
            excl_valid = 0;
            set_reg(rt2, ok ? 0 : 1);
        }
        return;
    }
    /* op1 == 1 */
    if (op2 == 1 && (op3 == 0 || op3 == 1)) {   /* TBB, TBH */
        u32 base = rn == 15 ? pc + 4 : R[rn];
        u32 off = op3 ? 2 * mem_ld(base + 2 * R[rd], 2, 0)
                      : 2 * mem_ld(base + R[rd], 1, 0);
        if (!fault_exc)
            branch_to(pc + 4 + off);
        return;
    }
    if (op3 == 4 || op3 == 5) {         /* LDREXB/H, STREXB/H */
        int n = op3 == 4 ? 1 : 2;
        u32 a = R[rn];
        if (op2 == 1) {
            u32 v = mem_ld(a, n, 1);
            if (fault_exc)
                return;
            set_reg(rt, v);
            excl_valid = 1;
            excl_addr = a;
        } else {
            int ok = excl_valid && excl_addr == a;
            if (!aligned_ok(a, n, 1))
                return;
            if (ok)
                st(a, n, R[rt]);
            if (fault_exc)
                return;
            excl_valid = 0;
            set_reg(rd, ok ? 0 : 1);
        }
        return;
    }
    undef();
}

static void dp_register(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 4 & 15, op2 = h2 >> 4 & 15;
    int rn = (int)(h1 & 15), rd = (int)(h2 >> 8 & 15), rm = (int)(h2 & 15);
    if ((h2 & 0xf000) != 0xf000) {
        undef();
        return;
    }
    if (op2 == 0 && op1 < 8) {          /* LSL LSR ASR ROR register */
        int c;
        u32 r = shift_c(R[rn], (int)(op1 >> 1), R[rm] & 0xff, C, &c);
        set_reg(rd, r);
        if (op1 & 1) {
            nz(r);
            C = c;
        }
        return;
    }
    if (op1 < 6 && (op2 & 8)) {         /* extend, and extend and add */
        u32 x = ror32(R[rm], 8 * (op2 & 3)), r;
        switch (op1) {
        case 0: r = (u32)(s32)(int16_t)x; break;
        case 1: r = x & 0xffff; break;
        case 4: r = (u32)(s32)(int8_t)x; break;
        case 5: r = x & 0xff; break;
        default:
            undef();                    /* SXTB16, UXTB16 */
            return;
        }
        if (rn != 15)
            r += R[rn];
        set_reg(rd, r);
        return;
    }
    if (op1 >= 8 && op1 < 12 && (op2 & 0xc) == 8) {
        u32 m = R[rm], r;
        int sat;
        switch (op1 << 4 | (op2 & 3)) {
        case 0x80: case 0x81: case 0x82: case 0x83: {   /* QADD etc */
            if (!cm->dsp) {
                undef();
                return;
            }
            s64 n = (s32)R[rn], mm = (s32)m;
            int s1 = 0;
            if (op2 & 1) {              /* QDADD, QDSUB double Rn */
                n = (s32)sat_signed(2 * n, 32, &s1);
            }
            s64 x = (op2 & 2) ? mm - n : mm + n;
            r = sat_signed(x, 32, &sat);
            if (sat || s1)
                Q = 1;
            break;
        }
        case 0x90:
            r = m >> 24 | (m >> 8 & 0xff00) | (m << 8 & 0xff0000) | m << 24;
            break;
        case 0x91:
            r = (m >> 8 & 0x00ff00ffu) | (m << 8 & 0xff00ff00u);
            break;
        case 0x92:
            r = 0;
            for (int i = 0; i < 32; i++)
                r |= (m >> i & 1) << (31 - i);
            break;
        case 0x93:
            r = (u32)(s32)(int16_t)((m >> 8 & 0xff) | (m & 0xff) << 8);
            break;
        case 0xa0: {                    /* SEL */
            if (!cm->dsp) {
                undef();
                return;
            }
            u32 n = R[rn];
            r = 0;
            for (int b = 0; b < 4; b++)
                r |= ((GE >> b & 1) ? n : m) & (0xffu << (8 * b));
            break;
        }
        case 0xb0:
            r = 0;
            while (r < 32 && !(m >> (31 - r) & 1))
                r++;
            break;
        default:
            undef();
            return;
        }
        set_reg(rd, r);
        return;
    }
    undef();                            /* parallel add and subtract */
}

static s32 half(u32 v, int top)
{
    return top ? (s32)(int16_t)(v >> 16) : (s32)(int16_t)v;
}

static void multiply(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 4 & 7, op2 = h2 >> 4 & 3;
    int rn = (int)(h1 & 15), ra = (int)(h2 >> 12 & 15);
    int rd = (int)(h2 >> 8 & 15), rm = (int)(h2 & 15);
    u32 a = R[rn], b = R[rm];
    if (op1 == 0) {
        if (op2 == 0)
            set_reg(rd, ra == 15 ? a * b : a * b + R[ra]);
        else if (op2 == 1)
            set_reg(rd, R[ra] - a * b);
        else
            undef();
        return;
    }
    if (!cm->dsp) {
        undef();
        return;
    }
    switch (op1) {
    case 1: {                           /* SMULxy, SMLAxy */
        s32 p = half(a, (int)(op2 >> 1 & 1)) * half(b, (int)(op2 & 1));
        if (ra == 15)
            set_reg(rd, (u32)p);
        else {
            s64 r = (s64)p + (s32)R[ra];
            if (r != (s32)r)
                Q = 1;
            set_reg(rd, (u32)r);
        }
        return;
    }
    case 3: {                           /* SMULWy, SMLAWy */
        s64 p = ((s64)(s32)a * half(b, (int)(op2 & 1))) >> 16;
        if (op2 & 2) {
            undef();
            return;
        }
        if (ra == 15)
            set_reg(rd, (u32)p);
        else {
            s64 r = p + (s32)R[ra];
            if (r != (s32)r)
                Q = 1;
            set_reg(rd, (u32)r);
        }
        return;
    }
    case 5: case 6: {                   /* SMMUL, SMMLA, SMMLS */
        s64 p = (s64)(s32)a * (s32)b;
        s64 acc = ra == 15 ? 0 : (s64)((u64)R[ra] << 32);
        s64 r = op1 == 6 ? acc - p : acc + p;
        if (op2 & 1)
            r += 0x80000000ll;
        set_reg(rd, (u32)((u64)r >> 32));
        return;
    }
    }
    undef();
}

static void long_mul_div(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 4 & 7, op2 = h2 >> 4 & 15;
    int rn = (int)(h1 & 15), lo = (int)(h2 >> 12 & 15);
    int hi = (int)(h2 >> 8 & 15), rm = (int)(h2 & 15);
    u32 a = R[rn], b = R[rm];
    if (op1 == 1 || op1 == 3) {         /* SDIV, UDIV */
        if (op2 != 15) {
            undef();
            return;
        }
        u32 r;
        if (b == 0) {
            if (ccr & 0x10) {
                fault_raise(EXC_USAGE, UFSR_DIVBYZERO);
                return;
            }
            r = 0;
        } else if (op1 == 1)
            r = (a == 0x80000000u && b == 0xffffffffu)
                    ? a : (u32)((s32)a / (s32)b);
        else
            r = a / b;
        set_reg(hi, r);
        return;
    }
    u64 acc = (u64)R[hi] << 32 | R[lo], r;
    switch (op1 << 4 | op2) {
    case 0x00: r = (u64)((s64)(s32)a * (s32)b); break;
    case 0x20: r = (u64)a * b; break;
    case 0x40: r = acc + (u64)((s64)(s32)a * (s32)b); break;
    case 0x60: r = acc + (u64)a * b; break;
    case 0x66:                          /* UMAAL */
        if (!cm->dsp) {
            undef();
            return;
        }
        r = (u64)a * b + R[lo] + R[hi];
        break;
    default:
        if (cm->dsp && op1 == 4 && (op2 & 0xc) == 8) {  /* SMLALxy */
            r = acc + (u64)(s64)(half(a, (int)(op2 >> 1 & 1)) *
                                 half(b, (int)(op2 & 1)));
            break;
        }
        undef();
        return;
    }
    set_reg(lo, (u32)r);
    set_reg(hi, (u32)(r >> 32));
}

static void exec32(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 11 & 3;
    if (cm->arch == ARCH_V6M) {
        /* BL, MSR, MRS, the barriers and UDF.W are all ARMv6-M has */
        if (op1 != 2 || !(h2 & 0x8000)) {
            undef();
            return;
        }
        branch_misc(h1, h2);
        return;
    }
    if (op1 == 1) {
        if ((h1 & 0x0640) == 0x0000) {  /* load/store multiple */
            u32 op = h1 >> 7 & 3;
            int W = (int)(h1 >> 5 & 1), L = (int)(h1 >> 4 & 1);
            int rn = (int)(h1 & 15);
            if (op == 0 || op == 3) {
                undef();
                return;
            }
            if (L && W && (h2 >> rn & 1))
                W = 0;
            ldst_mult(rn, h2, L, W, op == 2);
            return;
        }
        if ((h1 & 0x0640) == 0x0040) {
            ldst_dual_excl(h1, h2);
            return;
        }
        if ((h1 & 0x0600) == 0x0200) {  /* data processing, shifted reg */
            int t, sc;
            u32 n;
            imm_shift((int)(h2 >> 4 & 3), (h2 >> 12 & 7) << 2 | (h2 >> 6 & 3),
                      &t, &n);
            u32 op = h1 >> 5 & 15;
            if (op == 6) {              /* PKHBT, PKHTB */
                if (!cm->dsp) {
                    undef();
                    return;
                }
                u32 m = shift(R[h2 & 15], t, n, C), a = R[h1 & 15], r;
                if (h2 & 0x20)
                    r = (a & 0xffff0000u) | (m & 0xffff);
                else
                    r = (m & 0xffff0000u) | (a & 0xffff);
                set_reg((int)(h2 >> 8 & 15), r);
                return;
            }
            u32 b = shift_c(reg((int)(h2 & 15)), t, n, C, &sc);
            dp_op(op, (int)(h1 >> 4 & 1), (int)(h1 & 15), (int)(h2 >> 8 & 15),
                  b, sc);
            return;
        }
        /* coprocessor: the FPU, cp10 and cp11 */
        if ((h2 >> 9 & 7) != 5) {
            undef();
            return;
        }
        if ((h1 & 0x0e00) == 0x0c00) {
            vfp_ldst(h1, h2);
            return;
        }
        if ((h1 & 0x0f00) == 0x0e00) {
            if (h2 & 0x10)
                vfp_xfer(h1, h2);
            else
                vfp_dp(h1, h2);
            return;
        }
        undef();
        return;
    }
    if (op1 == 2) {
        if (h2 & 0x8000) {
            branch_misc(h1, h2);
            return;
        }
        if (h1 & 0x0200) {
            plain_imm(h1, h2);
            return;
        }
        int sc;
        u32 imm12 = (h1 >> 10 & 1) << 11 | (h2 >> 12 & 7) << 8 | (h2 & 0xff);
        u32 b = expand_imm_c(imm12, C, &sc);
        dp_op(h1 >> 5 & 15, (int)(h1 >> 4 & 1), (int)(h1 & 15),
              (int)(h2 >> 8 & 15), b, sc);
        return;
    }
    /* op1 == 3 */
    if ((h1 & 0x0e00) == 0x0800) {      /* 1111 100x: loads, stores */
        ldst_single(h1, h2);
        return;
    }
    if ((h1 & 0x0f00) == 0x0a00) {
        dp_register(h1, h2);
        return;
    }
    if ((h1 & 0x0f80) == 0x0b00) {
        multiply(h1, h2);
        return;
    }
    if ((h1 & 0x0f80) == 0x0b80) {
        long_mul_div(h1, h2);
        return;
    }
    if ((h1 & 0x0c00) == 0x0c00 && (h2 >> 9 & 7) == 5) {
        /* the FPv5 forms with the T bit (VSEL, VMAXNM, VRINT...): not
         * modelled yet */
        undef();
        return;
    }
    undef();
}

/* ---- the run --------------------------------------------------------- */

/* WFI and WFE: wait for an exception. When SysTick will raise one, skip
 * ahead to it; when nothing can, the run is over. */
static int wfx_idle(void)
{
    for (int n = 2; n < NEXC; n++)
        if (pend[n] && (n < 16 || irq_en[n]))
            return 0;
    if ((st_csr & 3) == 3) {
        u32 left = st_cvr ? st_cvr : st_rvr + 1;
        cycles += left;
        systick_advance(left);
        return 0;
    }
    state = END_IDLE;
    snprintf(end_why, sizeof end_why,
             "waiting for an interrupt that nothing can raise, at 0x%08x", pc);
    return 1;
}

static void step(void)
{
    u32 snap[16];
    int sN = N, sZ = Z, sC = C, sV = V, sQ = Q;
    u32 sit = itstate, sctl = control, sother = other_sp;
    int spsp = psp_active;

    if (npend) {
        int n = pending_to_take();
        if (n) {
            exc_entry(n, pc);
            return;
        }
    }
    if (!tbit) {
        take_sync(EXC_USAGE, UFSR_INVSTATE, pc);
        return;
    }
    u8 b[4];
    struct region *r = region_of(pc, 2);
    if (!r) {
        take_sync(EXC_BUS, BFSR_IBUSERR, pc);
        return;
    }
    memcpy(b, r->mem + (pc - r->base), 2);
    u32 h1 = (u32)b[0] | (u32)b[1] << 8, h2 = 0;
    int size = 2;
    if ((h1 >> 11) >= 0x1d) {
        struct region *r2 = region_of(pc + 2, 2);
        if (!r2) {
            take_sync(EXC_BUS, BFSR_IBUSERR, pc);
            return;
        }
        memcpy(b + 2, r2->mem + (pc + 2 - r2->base), 2);
        h2 = (u32)b[2] | (u32)b[3] << 8;
        size = 4;
    }
    int br;
    u32 cost = (u32)arm_cost(b, (size_t)size, &br);
    insns++;
    cycles += cost;
    if (trace) {
        if (size == 4)
            fprintf(trace_out, "%08x: %04x %04x\n", pc, h1, h2);
        else
            fprintf(trace_out, "%08x: %04x\n", pc, h1);
    }

    memcpy(snap, R, sizeof snap);
    npc = pc + (u32)size;
    pc_written = 0;
    exc_return_pending = 0;
    fault_exc = 0;
    int is_it = size == 2 && (h1 & 0xff00) == 0xbf00 && (h1 & 15);
    int was_in_it = in_it();
    int run_it = !was_in_it || cond_passed(itstate >> 4);
    if (run_it) {
        if (size == 2)
            exec16(h1);
        else
            exec32(h1, h2);
    }
    if (fault_exc) {
        int exc = fault_exc;
        u32 bits = fault_bits;
        memcpy(R, snap, sizeof snap);
        N = sN; Z = sZ; C = sC; V = sV; Q = sQ;
        itstate = sit;
        if (control != sctl || psp_active != spsp) {
            use_psp(spsp);
            control = sctl;
        }
        other_sp = sother;
        fault_exc = 0;
        if (exc == EXC_BUS && fault_addr_valid) {
            bfar = fault_addr;
            bits |= 1u << 15;
        }
        if (exc == EXC_SVC) {
            /* SVC completes: it returns past itself */
            if (was_in_it && !is_it)
                it_advance();
            take_sync(EXC_SVC, 0, npc);
            return;
        }
        take_sync(exc, bits, pc);
        return;
    }
    if (was_in_it && !is_it)
        it_advance();
    if (br && npc != pc + (u32)size)
        cycles += 2;
    systick_advance(cost);
    if (exc_return_pending) {
        exc_return(exc_return_value);
        return;
    }
    if (pc_written && npc == pc && !in_it() && state == RUN) {
        /* a branch to itself: the end unless an exception can come */
        int can = (st_csr & 3) == 3;
        for (int n = 2; n < NEXC && !can; n++)
            if (pend[n])
                can = 1;
        if (!can) {
            state = END_IDLE;
            snprintf(end_why, sizeof end_why,
                     "a loop at 0x%08x that nothing can interrupt", pc);
            return;
        }
    }
    pc = npc;
}

/* ---- loading the image ----------------------------------------------- */

static u8 *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open %s", path);
    size_t cap = 1 << 16, n = 0;
    u8 *buf = malloc(cap);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        if (!buf)
            die("out of memory reading %s", path);
        size_t got = fread(buf + n, 1, cap - n, f);
        if (!got)
            break;
        n += got;
    }
    fclose(f);
    *len = n;
    return buf;
}

static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return le16(p) | le16(p + 2) << 16; }

/* Every PT_LOAD segment at its physical (load) address, as QEMU's
 * -kernel and a flash programmer place it. A segment outside the board's
 * memory gets a region of its own, so a link script for a slightly
 * different part still runs. */
static void load_elf(const char *path)
{
    size_t len;
    u8 *f = slurp(path, &len);
    if (len < 52 || memcmp(f, "\177ELF", 4) != 0)
        die("%s is not an ELF file", path);
    if (f[4] != 1 || f[5] != 1)
        die("%s is not a 32-bit little-endian ELF image", path);
    if (le16(f + 18) != 40)
        die("%s is not an ARM image (e_machine %u)", path, le16(f + 18));
    if (le16(f + 16) != 2)
        die("%s is not a linked image (link it first)", path);
    u32 phoff = le32(f + 28), phentsize = le16(f + 42), phnum = le16(f + 44);
    int loaded = 0;
    for (u32 i = 0; i < phnum; i++) {
        const u8 *ph = f + phoff + i * phentsize;
        if ((size_t)(ph - f) + 32 > len)
            die("%s: truncated program headers", path);
        if (le32(ph) != 1)
            continue;
        u32 off = le32(ph + 4), pa = le32(ph + 12);
        u32 fsz = le32(ph + 16);
        if (!fsz)
            continue;
        if ((size_t)off + fsz > len)
            die("%s: a segment runs past the end of the file", path);
        if (!region_of(pa, fsz)) {
            u32 base = pa & ~0xfffu, end = (pa + fsz + 0xfffu) & ~0xfffu;
            add_region(base, end - base, 0);
        }
        for (u32 k = 0; k < fsz; k++) {
            struct region *r = region_of(pa + k, 1);
            if (!r)
                die("%s: a segment crosses 0x%08x, outside memory", path,
                    pa + k);
            r->mem[pa + k - r->base] = f[off + k];
        }
        loaded++;
    }
    free(f);
    if (!loaded)
        die("%s has nothing to load", path);
}

static void reset(void)
{
    ccr = cm->arch == ARCH_V7M ? 0x200u : 0x208u;   /* STKALIGN */
    vtor = 0;
    u32 sp = ld(0, 4), entry = ld(4, 4);
    if (fault_exc)
        die("the vector table at 0 cannot be read");
    R[13] = sp & ~3u;
    R[14] = 0xffffffffu;
    tbit = (int)(entry & 1);
    pc = entry & ~1u;
    for (int n = 4; n < NEXC; n++)
        prio[n] = 0;
}

static u32 parse_size(const char *s)
{
    char *e;
    unsigned long v = strtoul(s, &e, 0);
    if (*e == 'K' || *e == 'k')
        v <<= 10, e++;
    else if (*e == 'M' || *e == 'm')
        v <<= 20, e++;
    if (*e || e == s)
        die("bad size '%s'", s);
    return (u32)v;
}

static void usage(void)
{
    fputs("usage: embsim IMAGE.elf [--board NAME] [--cpu NAME]\n"
          "              [--ram-size SIZE] [--until STRING] [--max-insns N]\n"
          "              [--stats] [--count FILE] [--trace FILE]\n"
          "              [--no-semihosting]\n"
          "boards: lm3s6965evb (default), mps2-an385, mps2-an386,\n"
          "        mps2-an500, microbit\n"
          "cpus:   cortex-m0, cortex-m0plus, cortex-m3, cortex-m4, cortex-m7\n",
          stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *image = 0, *cpu = 0, *count_path = 0, *trace_path = 0;
    u32 ram_size = 0;
    int stats = 0;
    bd = &boards[0];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--board") && more) {
            const char *nm = argv[++i];
            bd = 0;
            for (size_t k = 0; k < sizeof boards / sizeof boards[0]; k++)
                if (!strcmp(boards[k].name, nm))
                    bd = &boards[k];
            if (!bd)
                die("unknown board '%s'", nm);
        } else if (!strcmp(a, "--cpu") && more)
            cpu = argv[++i];
        else if (!strcmp(a, "--ram-size") && more)
            ram_size = parse_size(argv[++i]);
        else if (!strcmp(a, "--until") && more) {
            until = argv[++i];
            until_len = strlen(until);
            if (!until_len)
                until = 0;
        } else if (!strcmp(a, "--max-insns") && more)
            max_insns = strtoull(argv[++i], 0, 0);
        else if (!strcmp(a, "--count") && more)
            count_path = argv[++i];
        else if (!strcmp(a, "--trace") && more)
            trace_path = argv[++i];
        else if (!strcmp(a, "--stats"))
            stats = 1;
        else if (!strcmp(a, "--verbose") || !strcmp(a, "-v"))
            verbose = 1;
        else if (!strcmp(a, "--no-semihosting"))
            semihosting = 0;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h"))
            usage();
        else if (a[0] == '-')
            die("unknown option '%s'", a);
        else if (!image)
            image = a;
        else
            die("one image at a time ('%s' and '%s')", image, a);
    }
    if (!image)
        usage();
    if (!cpu)
        cpu = bd->cpu;
    for (size_t k = 0; k < sizeof cpus / sizeof cpus[0]; k++)
        if (!strcmp(cpus[k].name, cpu))
            cm = &cpus[k];
    if (!cm)
        die("unknown cpu '%s'", cpu);
    if (trace_path) {
        trace_out = strcmp(trace_path, "-") ? fopen(trace_path, "w") : stderr;
        if (!trace_out)
            die("cannot write %s", trace_path);
        trace = 1;
    }

    add_region(bd->flash, bd->flash_size, 1);
    add_region(bd->ram, ram_size ? ram_size : bd->ram_size, 0);
    add_region(bd->ram2, bd->ram2_size, 0);
    /* the image is written into "flash" before the run makes it ROM */
    rg[0].rom = 0;
    load_elf(image);
    rg[0].rom = bd->flash_rom;
    reset();

    while (state == RUN) {
        step();
        if (max_insns && insns >= max_insns && state == RUN) {
            state = END_BUDGET;
            snprintf(end_why, sizeof end_why,
                     "--max-insns: %llu instructions run",
                     (unsigned long long)insns);
        }
    }
    fflush(stdout);
    if (count_path) {
        FILE *f = fopen(count_path, "w");
        if (!f)
            die("cannot write %s", count_path);
        fprintf(f, "%llu\n%llu\n", (unsigned long long)insns,
                (unsigned long long)cycles);
        fclose(f);
    }
    if (stats || verbose || state == END_LOCKUP || state == END_BUDGET)
        fprintf(stderr, "embsim: %s; %llu instructions, %llu cycles (est.)\n",
                end_why, (unsigned long long)insns,
                (unsigned long long)cycles);
    if (trace_out && trace_out != stderr)
        fclose(trace_out);
    switch (state) {
    case END_EXIT: return exit_status;
    case END_LOCKUP: return 3;
    case END_BUDGET: return 4;
    default: return 0;
    }
}
