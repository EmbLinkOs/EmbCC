/* sim.h -- EmbSim's shared types and the interfaces between its modules.
 *
 * EmbSim is a machine put together from parts, each behind an interface
 * this file declares (docs/internals/embsim.md is the contributor's
 * guide):
 *
 *   - a CPU (struct cpu_ops): reset, step one instruction, the registers
 *     by GDB number, the target description, and interrupt entry. The
 *     Cortex-M is cortexm.c (with cortexm-thumb.c and cortexm-fpu.c).
 *   - the bus (bus.c): memory regions (RAM, flash that ignores stores),
 *     bit-band aliases, and memory-mapped devices by address range, each
 *     with read and write callbacks and a hook for the time that passes
 *     (struct dev_ops). The UARTs, SysTick, the system control space and
 *     DWT are devices.
 *   - boards as data (boards.c): a core, its options, the memory regions
 *     and the devices at their addresses. A new board is a table entry.
 *   - the run (run.c): the loop, time, the end-of-run rules and the
 *     console; semihost.c, trace.c (tracing and the counts), loader.c
 *     (the ELF image), gdb.c (the GDB remote server, over net.h's
 *     connection), and main.c (the command line).
 *
 * The C is ISO C99 with no dependencies beyond libm; net-posix.c is the
 * one file that uses an operating system's API, behind net.h. */
#ifndef EMBSIM_SIM_H
#define EMBSIM_SIM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

struct sim;
struct cpu;

/* print "embsim: MESSAGE" and exit with status 2 */
void die(const char *fmt, ...);

/* ---- the bus (bus.c) -------------------------------------------------- */

/* MEM_ALIAS: another name for the region at a board's `target` (the
 * STM32's flash, seen at 0 as well as at 0x08000000) */
enum { MEM_RAM, MEM_FLASH, MEM_ALIAS };

struct region {
    u32 base, size;
    u8 *mem;
    int rom;                        /* stores are ignored */
    int kind;                       /* MEM_RAM or MEM_FLASH */
};

/* A memory-mapped device. `off` is the offset of the access from the
 * device's base, `n` its size in bytes (1, 2 or 4); a read's value is
 * masked to n bytes by the bus. Any hook may be 0. */
struct dev_ops {
    const char *type;
    u32 (*read)(void *ctx, u32 off, int n);
    void (*write)(void *ctx, u32 off, int n, u32 v);
    void (*reset)(void *ctx);
    /* `cycles` of time have passed. Called after every instruction, but
     * only while the device has said it is counting (sim_clock): a
     * stopped timer costs nothing. */
    void (*tick)(void *ctx, u32 cycles);
    /* 1 and the cycles until the device next raises an interrupt, or 0
     * when it will not without the program doing something */
    int (*next_event)(void *ctx, u32 *cycles);
};

struct device {
    const struct dev_ops *ops;
    void *ctx;
    u32 base, size;
};

/* a bit-band alias: one word at base + 32 * byte + 4 * bit for each bit
 * of the `size / 32` bytes at target */
struct alias {
    u32 base, size, target;
};

/* a GDB watchpoint: Z2, Z3 or Z4 */
enum { WATCH_WRITE = 2, WATCH_READ = 3, WATCH_ACCESS = 4 };
struct watch {
    u32 addr, len;
    int kind;
};

#define BUS_REGIONS 8
#define BUS_DEVICES 16
#define BUS_ALIASES 4
#define BUS_WATCHES 32

struct bus {
    struct region rg[BUS_REGIONS];
    int nrg;
    struct device dev[BUS_DEVICES];
    int ndev;
    struct alias bb[BUS_ALIASES];
    int nbb;
    u32 fail_addr;                  /* the address of a failed access */
    /* every store the core makes, as the exclusive monitor sees it */
    void (*snoop)(void *ctx, u32 a);
    void *snoop_ctx;
    struct watch watch[BUS_WATCHES];
    int nwatch;
    int watch_hit, watch_kind;      /* the first watchpoint the core hit */
    u32 watch_addr;
    int debug;                      /* the access is a debugger's */
    int dev_fault;                  /* the device refused it: bus_fault */
};

void bus_add_region(struct bus *b, u32 base, u32 size, int kind);
/* the region holding all `n` bytes at `a`, or 0: memory's fast path, so
 * inline (the core fetches through it, and reads and writes memory
 * through it when no watchpoint is set) */
static inline struct region *bus_region(struct bus *b, u32 a, u32 n)
{
    for (int i = 0; i < b->nrg; i++) {
        struct region *r = &b->rg[i];
        if (a - r->base < r->size && r->size - (a - r->base) >= n)
            return r;
    }
    return 0;
}
static inline u32 mem_rd_le(const u8 *p, int n)
{
    u32 v = p[0];
    if (n > 1)
        v |= (u32)p[1] << 8;
    if (n > 2)
        v |= (u32)p[2] << 16 | (u32)p[3] << 24;
    return v;
}
static inline void mem_wr_le(u8 *p, int n, u32 v)
{
    p[0] = (u8)v;
    if (n > 1)
        p[1] = (u8)(v >> 8);
    if (n > 2) {
        p[2] = (u8)(v >> 16);
        p[3] = (u8)(v >> 24);
    }
}
void bus_add_device(struct bus *b, u32 base, u32 size,
                    const struct dev_ops *ops, void *ctx);
void bus_add_alias(struct bus *b, u32 base, u32 size, u32 target);
/* a second name for the memory region holding `target`, at `base` */
void bus_add_mirror(struct bus *b, u32 base, u32 target);
/* a device's read or write hook refuses the access in progress: the
 * core takes its bus fault, as when nothing answers */
static inline void bus_fault(struct bus *b)
{
    b->dev_fault = 1;
}
/* The core's accesses: 0, or -1 with bus->fail_addr when nothing answers
 * at the address. A store tells the snoop. */
int bus_read(struct bus *b, u32 a, int n, u32 *v);
int bus_write(struct bus *b, u32 a, int n, u32 v);
/* Whether an access of the core's touches a watchpoint (1), recording
 * the first that one did in watch_hit. The core asks before the access,
 * so it can stop before it as QEMU's stub does. */
int bus_watch_check(struct bus *b, u32 a, int n, int write);
/* A debugger's: no watchpoints, no exclusive monitor, and a write to
 * flash goes in (as a flash programmer's does). */
int bus_debug_read(struct bus *b, u32 a, int n, u32 *v);
int bus_debug_write(struct bus *b, u32 a, int n, u32 v);
int bus_watch_add(struct bus *b, int kind, u32 addr, u32 len);
int bus_watch_remove(struct bus *b, int kind, u32 addr, u32 len);

/* ---- a CPU ------------------------------------------------------------ */

struct cpu_ops {
    const char *name;               /* "cortex-m" */
    int elf_machine;                /* the e_machine of its images */
    const char *elf_name;           /* "an ARM image" */
    /* power-on reset: the core's state and its own devices, then the
     * reset sequence (the vector table, for a Cortex-M) */
    void (*reset)(struct cpu *c);
    /* one instruction, or the entry to an exception */
    void (*step)(struct cpu *c);
    u32 (*pc)(struct cpu *c);
    /* the registers by GDB number: the bytes written, 0 when there is no
     * such register */
    int (*reg_read)(struct cpu *c, int n, u8 *buf);
    int (*reg_write)(struct cpu *c, int n, const u8 *buf);
    /* the registers of a `g` packet, in order, ending with -1 */
    const int *(*gdb_g_regs)(struct cpu *c);
    /* the target description (qXfer:features:read) for `annex`, or 0 */
    const char *(*gdb_xml)(struct cpu *c, const char *annex);
    /* the size of the breakpoint instruction at an address (Z0's kind) */
    int bp_kind;
    /* make exception or interrupt `n` pending (an ARM exception number:
     * 15 is SysTick, 16 + k is external interrupt k) */
    void (*interrupt)(struct cpu *c, int n);
    /* the program counter's GDB number (`c ADDR` and `s ADDR` set it) */
    int pc_regnum;
    /* the ELF classes its images may be: 1 for ELFCLASS32, 2 for
     * ELFCLASS64, or both (RISC-V) */
    int elf_classes;
};

struct cpu {
    const struct cpu_ops *ops;
};

/* ---- boards (boards.c) ------------------------------------------------ */

struct mem_desc {
    u32 base, size;
    int kind;                       /* MEM_RAM, MEM_FLASH or MEM_ALIAS */
    int main_ram;                   /* the one --ram-size resizes */
    u32 target;                     /* MEM_ALIAS: the region it names */
};

struct dev_desc {
    const char *type;               /* a name in boards.c's device list */
    u32 base, size;
};

struct board_desc {
    const char *name;
    const char *core;               /* a name in boards.c's core list */
    const char *cpu;                /* the core's model */
    int prio_bits;                  /* NVIC priority bits implemented */
    struct mem_desc mem[4];         /* ends with a size of 0 */
    struct dev_desc dev[6];         /* ends with a type of 0 */
    struct alias bitband[2];        /* ends with a size of 0 */
    /* the part's CMSIS-SVD file, by its name: its peripherals' registers
     * are on the bus (svd-map.c), found with --svd or EMBSIM_SVD_PATH */
    const char *svd;
    /* the behavioural models layered over those registers, by the SVD's
     * peripheral names; ends with a name of 0 */
    const struct model_desc *models;
};

/* a model for the SVD's peripherals whose names match `periph` (a `*`
 * matches any characters) */
struct model_desc {
    const char *periph;
    const char *model;              /* a name in boards.c's model list */
};

struct core_type {
    const char *name;
    /* the model's core, or 0 when the model is not one of this core's */
    struct cpu *(*create)(struct sim *s, const char *model,
                          const struct board_desc *bd);
};

struct dev_type {
    const char *type;
    const struct dev_ops *ops;
    void *(*create)(struct sim *s, const struct dev_desc *d);
};

extern const struct board_desc boards[];
extern const int nboards;
extern const struct core_type cores[];
extern const int ncores;
const struct board_desc *board_find(const char *name);
void sim_add_device(struct sim *s, const struct dev_desc *d);
/* a device on the bus, keeping time if it has a clock (a tick or a
 * next_event hook) */
void sim_add_dev(struct sim *s, u32 base, u32 size, const struct dev_ops *ops,
                 void *ctx);

/* ---- the run (run.c) -------------------------------------------------- */

enum { RUN, END_EXIT, END_LOCKUP, END_IDLE, END_UNTIL, END_BUDGET };

#define SIM_TICKERS 8

struct sim {
    struct bus bus;
    struct cpu *cpu;
    const struct board_desc *board;
    const char *image;
    u64 insns, cycles;
    int state, exit_status;
    char end_why[160];
    const char *until;              /* stop once the output holds this */
    size_t until_len, until_at;     /* how much of it has matched */
    u64 max_insns;
    int semihosting;
    FILE *trace;                    /* --trace, or 0 */
    /* the devices that keep time: their tick hooks, flat, since they
     * are called after every instruction; and the devices themselves */
    void (*tick_fn[SIM_TICKERS])(void *ctx, u32 cycles);
    void *tick_ctx[SIM_TICKERS];
    int ntick_fn;
    struct device *tick[SIM_TICKERS];
    int ntick;
    int counting;                   /* devices whose clock is running */
    /* the image, as the loader found it: its entry point, whether it is
     * ELFCLASS64, and its e_flags (a core with more than one width or
     * ABI reads them at reset) */
    u64 entry;
    int elf64;
    u32 elf_flags;
    /* instructions a skip passed over (AVR's CPSE, SBRC, SBRS, SBIC and
     * SBIS): not run, so not in `insns`; with count_skips, --count's
     * file has them as a third line */
    u64 skipped;
    int count_skips;
    /* the SVD's register file (svd-map.c), or 0 */
    struct svdmap *svd;
};

/* build the board with its core (`model`, or 0 for the board's own),
 * and with the peripherals of an SVD file (`svd`, or 0 for the board's
 * own, if it has one) */
void sim_init(struct sim *s, const struct board_desc *bd, const char *model,
              const char *svd);
/* the memory (`ram_size`, or 0 for the board's), the image in it, and
 * the core out of reset */
void sim_load(struct sim *s, u32 ram_size, const char *image);
/* a reset, as the reset pin gives one: the core and the devices, and
 * the counts back to zero; memory as it is, or (reload) the image loaded
 * into it again */
void sim_reset(struct sim *s, int reload);
/* one instruction, and the budget (--max-insns) */
void sim_step(struct sim *s);
/* run until the end */
void sim_run(struct sim *s);
/* end the run: state, and the reason --stats prints */
void sim_end(struct sim *s, int state, const char *fmt, ...);
/* a byte of the program's output */
void sim_out(struct sim *s, int c);
/* `cycles` of time pass for the devices */
void sim_advance(struct sim *s, u32 cycles);
/* a device's clock starts (on 1) or stops (0): whether its tick hook
 * needs calling; `*running` is the device's own record of it */
void sim_clock(struct sim *s, int *running, int on);
/* 1 and the cycles to the next device interrupt, or 0 when none is due */
int sim_next_event(struct sim *s, u32 *cycles);

/* ---- semihosting (semihost.c) ----------------------------------------- */

/* the core's view of memory, for a semihosting call: an access that
 * faults sets the core's fault, and `faulted` reports it */
struct semi_ops {
    u32 (*ld)(void *ctx, u32 a, int n);
    void (*st)(void *ctx, u32 a, int n, u32 v);
    int (*faulted)(void *ctx);
};

/* An ARM semihosting call: operation `op` (r0), parameter `arg` (r1).
 * Returns 1 with the result for r0 in *ret, or 0 when r0 is unchanged. */
int semihost(struct sim *s, const struct semi_ops *m, void *ctx, u32 op,
             u32 arg, u32 *ret);

/* ---- tracing and counting (trace.c) ----------------------------------- */

void trace_open(struct sim *s, const char *path);
/* one instruction: its address and its units (halfwords for Thumb),
 * each `unit` bytes wide */
void trace_insn(struct sim *s, u32 pc, const u32 *units, int n, int unit);
/* the end of a run: --count's file, the --stats line, the trace closed */
void trace_report(struct sim *s, const char *count_path, int stats);

/* ---- the image (loader.c) --------------------------------------------- */

void load_elf(struct sim *s, const char *path);

/* ---- the GDB server (gdb.c) ------------------------------------------- */

/* Serve the GDB remote protocol on `port` of `host` (0: loopback) while
 * the run goes on; with `wait`, halted at reset until a debugger
 * connects. Returns when the run has ended (or the debugger killed it),
 * with s->state and s->exit_status saying how. */
void gdb_serve(struct sim *s, const char *host, int port, int wait);

#endif
