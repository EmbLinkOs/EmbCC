/* Register allocation, shared between the backends.
 *
 * D-011 said what would justify this: "the two backends start
 * duplicating real algorithms — a register allocator written twice is
 * the signal that the shared layer is in the wrong place, and the
 * answer then is to lift the machine-independent half out of
 * codegen.c, deriving the shared shape from two WORKING backends
 * rather than inventing it from one."
 *
 * That is what this is. The x86-64 backend's allocator had been
 * running for a while and aarch64 had none, so rather than write a
 * second one the working one was lifted. Nothing about it changed in
 * the move -- `tools/x86-identity.sh` was the check, and it compares
 * emitted BYTES rather than test results, so "nothing changed" is a
 * statement about the objects and not about the suite.
 *
 * ---- what is machine-independent, and what is not -------------------------
 *
 * The algorithm is: real backward liveness over the IR (spanning loop
 * back-edges, which an appearance interval does not), a precise
 * interference graph, Chaitin-Briggs simplify ordering, and colouring
 * with move-coalescing preferences. None of that knows what a register
 * is; it works in the IR and in indices.
 *
 * What a machine supplies is a `struct ra_target`: which registers may
 * be handed out and in what order of preference, which of them survive
 * a call, and whether a narrow load is a plain move on that machine.
 * Everything else the allocator needs it reads from the IR.
 */
#ifndef EMBCC_REGALLOC_H
#define EMBCC_REGALLOC_H

#include "../ir/ir.h"

/* The most registers any target here offers the allocator. Sizes the
 * per-colour arrays; a literal because EmbCC's own subset does not fold
 * a sizeof/sizeof there. */
#define RA_MAXPOOL 16

struct ra_target {
    /* The pool a normal function draws from, preferred first: put the
     * caller-saved registers first so a short-lived value takes one and
     * skips the prologue save. */
    const int *pool;
    int npool;
    /* And a variadic function's, which is smaller: a variadic prologue
     * spills the argument register file, so those registers are not
     * available. NULL means "the same as pool". */
    const int *pool_varargs;
    int npool_varargs;

    /* Does this register survive a call? A value whose live range
     * crosses a call may only take one that does. */
    int (*is_callee_saved)(int reg);

    /* Is `dst = load(local)` a plain register move on this machine --
     * no sign- or zero-extension emitted -- so the two may share a
     * register and the load disappear? Purely a property of the
     * machine's load instructions. */
    int (*ldvar_plain)(int size, int sign, int w);
};

/* Assign a register to every eligible vreg of `fn`, or -1 for one that
 * stays in memory. `wide` (may be NULL) marks vregs holding a value too
 * large for a register -- a long double -- which are never eligible.
 *
 * Returns a malloc'd array indexed by vreg; fills `used_out` with the
 * callee-saved registers the function actually took (so the prologue
 * knows what to save) and `*nused_out` with how many. The caller frees
 * the array. */
int *ra_allocate(struct ir_func *fn, const struct ra_target *t,
                 const char *wide, int *used_out, int *nused_out);

/* Backward liveness over the IR: fills first[v]/last[v] with the range
 * vreg v is live over -- a sound over-approximation that SPANS loop
 * back-edges, where a naive first/last-appearance interval does not and
 * would let a loop-carried value's register be clobbered mid-loop. Also
 * returns the per-instruction live-in and live-out bitsets and the def
 * vreg per instruction, which the interference graph needs.
 *
 * Shared because slot coalescing wants the same ranges the allocator
 * does, and two copies of a dataflow are two chances to disagree about
 * a back-edge. Everything is malloc'd; the caller frees. */
unsigned long *ra_live_intervals(struct ir_func *fn, int *first, int *last,
                                 unsigned long **livein_out, int **defv_out,
                                 int *words_out);

/* The vreg an instruction WRITES, or -1. In the shared layer because
 * liveness is: it has to agree with what the backends actually store,
 * and one copy is how it stays agreed. */
int ra_ins_def(const struct ir_ins *in);

#endif
