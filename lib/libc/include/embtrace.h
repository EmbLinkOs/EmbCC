/* EmbTrace: function-level tracing on the target.
 *
 * Compile the code to trace with -finstrument-functions; lib/rt records
 * every entry and exit in a ring in RAM (lib/rt/embtrace.c), and
 * embtrace_dump() writes it out through embtrace_putc, a character at a
 * time, for tools/embtrace on the host:
 *
 *     embtrace firmware.elf console.log            calls, times, the tree
 *     embtrace firmware.elf console.log --chrome t.json   for Perfetto
 *
 * Every part of the recorder is weak and replaceable. See
 * docs/manual/tools/embtrace.md. */
#ifndef EMBTRACE_H
#define EMBTRACE_H

#define EMBTRACE_NOI __attribute__((no_instrument_function))

#ifdef __cplusplus
extern "C" {
#endif

/* Write the ring out (oldest event first) through embtrace_putc. */
void embtrace_dump(void);
/* Forget every event recorded so far. */
void embtrace_reset(void);
/* Stop (0) or resume (1) recording. */
void embtrace_enable(int on);

/* Define these to replace the defaults (each is weak in lib/rt):
 *
 *   void embtrace_putc(int c);          where the dump goes (default: nowhere)
 *   unsigned embtrace_clock(void);      the time stamp (default: 0)
 *   const unsigned embtrace_clock_hz;   its rate, for times in seconds
 *   struct embtrace_ev embtrace_events[N];  and
 *   const unsigned embtrace_capacity = N;   a ring of another size
 *
 * Each must be compiled without -finstrument-functions, or marked
 * EMBTRACE_NOI. */
void embtrace_putc(int c);
unsigned embtrace_clock(void);

/* Ready-made clocks. A Cortex-M3 and up count cycles in the DWT, once it
 * is turned on; RISC-V counts them in mcycle (machine mode). */
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || \
    defined(__ARM_ARCH_8M_MAIN__)
static inline EMBTRACE_NOI void embtrace_dwt_start(void)
{
    *(volatile unsigned *)0xE000EDFCu |= 1u << 24;      /* DEMCR.TRCENA */
    *(volatile unsigned *)0xE0001004u = 0;              /* DWT_CYCCNT */
    *(volatile unsigned *)0xE0001000u |= 1u;            /* CYCCNTENA */
}
static inline EMBTRACE_NOI unsigned embtrace_dwt_cycles(void)
{
    return *(volatile unsigned *)0xE0001004u;
}
#endif
#if defined(__riscv)
static inline EMBTRACE_NOI unsigned embtrace_mcycle(void)
{
    unsigned long c;
    __asm__ volatile ("csrr %0, mcycle" : "=r"(c));
    return (unsigned)c;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
