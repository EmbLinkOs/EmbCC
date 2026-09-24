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

    /* ---- what the BACKEND can read out of a register ------------------
     *
     * These are not properties of the IR. They are things a particular
     * backend has been taught to do, and a backend that has not been
     * taught reads the value from its stack slot -- so the allocator
     * must leave it there.
     *
     * The distinction cost a miscompile to learn. The allocator was
     * lifted out of the x86 backend with these cases commented
     * "register-aware", which was true OF THAT BACKEND, and the flags
     * did not exist because there was only one. Handed to aarch64,
     * which reads a call's arguments and a memcpy's addresses straight
     * from their slots, it allocated values that were then read from
     * memory that nothing had written -- and tests/exec/aapcs64.c
     * returned 1 instead of 42.
     *
     * D-011 anticipated the shape of this: derive the shared layer from
     * two WORKING backends rather than one. A capability the second
     * backend lacks has to be sayable, or the shared layer is the first
     * backend wearing a hat. */
    int call_int_arg_in_reg;   /* a scalar-integer call argument */
    int ret_scalar_in_reg;     /* a scalar return value */
    int memcpy_addr_in_reg;    /* IR_MEMCPY / IR_MEMZERO address operands */
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

/* What a backend knows about slot assignment that this layer does not. */
struct ra_slots {
    /* Per-vreg physical register, or NULL when the allocator is off. A
     * vreg with one never touches memory, so it needs no slot -- which
     * is also what keeps mem2reg's SSA-version inflation out of the
     * frame. */
    const int *loc;
    /* Drop the slot of a temp that appears in NO instruction. Common
     * once immediate-folding detaches a CONST and dead-code removal
     * takes its definition; a throwaway eight bytes each inflates the
     * frame for nothing. Off at -O0, where the layout must not move. */
    int opt_frames;
    /* An indirect jump reaches any address-taken label, so a live range
     * measured by appearance is not a live range. No coalescing then. */
    int has_cgoto;
};

/* Assign each temp of `fn` a slot index in a shared pool, or -1 for one
 * that needs no slot. Returns a malloc'd array indexed by (vreg -
 * nvars), and fills *npool_out with how many slots the pool needs. */
int *ra_coalesce_temps(struct ir_func *fn, int nvars,
                       const struct ra_slots *o, int *npool_out);

/* Which LOCALS any instruction still names — one byte per slot, 1 when
 * the frame must hold it. A local nothing names needs no stack at all,
 * and SROA leaves exactly that behind: once every field access has
 * become a read or write of the scalars an aggregate was split into,
 * the aggregate itself is mentioned nowhere, and it would otherwise
 * keep its full size on the frame for the rest of the function.
 *
 * `want_debug` makes every slot referenced: -g hands the debugger an
 * address for each variable by name, whether the code reads it or not.
 * So do a varargs function (a va_list walks the incoming area) and an
 * alloca (it moves the stack out from under the layout), both read from
 * `fn`. Parameters always count: the PROLOGUE writes them, and that
 * store is not in the IR to be found here.
 *
 * Shared because it reads only the IR -- the two backends had the same
 * eight lines, and two copies of "which slots matter" are two chances
 * to disagree about one. The caller frees. */
char *ra_locals_referenced(const struct ir_func *fn, int want_debug);

/* Does local `v` need a stack slot at all, given the allocation `loc`?
 * Lifted here once BOTH backends wanted it (D-011): x86-64 had carried
 * it alone, and the aarch64 frame was paying for a slot behind every
 * value the allocator had already put in a register. */
int ra_slot_dead(const struct ir_func *fn, const int *loc, int v,
                 int want_debug);

/* The vreg an instruction WRITES, or -1. In the shared layer because
 * liveness is: it has to agree with what the backends actually store,
 * and one copy is how it stays agreed. */
int ra_ins_def(const struct ir_ins *in);

#endif
