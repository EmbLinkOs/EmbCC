/* The optimizer (VISION_LONGTERM, now begun). A small set of local IR
 * passes run per function between irgen and codegen. Everything is opt-in:
 * at level 0 opt_run does nothing, so -O0 output is byte-for-byte what it
 * was before the optimizer existed — which is what keeps the self-host
 * fixed point (self-host builds at -O0).
 *
 * The IR is a linear three-address form (ir.h) whose expression
 * temporaries are single-assignment: each new_temp result is written by
 * exactly one instruction. That is what makes these passes safe without a
 * full CFG/SSA construction — a value in a single-def vreg is invariant, so
 * constant folding, copy propagation, and dead-code elimination need only
 * per-function def/use counts.
 */
#ifndef EMBCC_OPT_OPT_H
#define EMBCC_OPT_OPT_H

#include "../ir/ir.h"

/* The level -Os asks for: level 2 without the passes that trade size
 * for speed. Spelled as a level so the driver has one path. */
#define OPT_SIZE (-2)

/* Optimize every function of the unit in place at the given level
 * (0 = none, >=1 = the local passes, 2 = the global and loop passes,
 * 3 = 2 plus the ones that trade size for speed). */
void opt_run(struct ir_unit *iu, int level);

/* ---- turning one pass off ------------------------------------------
 *
 * `-fno-<name>` / `-f<name>`, as gcc spells it. Two reasons this earns
 * its place rather than being a debugging convenience:
 *
 *   A wrong answer at -O2 is a question about WHICH pass, and the only
 *   way to ask it was to patch the compiler and rebuild. Hunting the
 *   computed-goto miscompile took a hand-rolled env var and five builds
 *   where it should have taken five command lines.
 *
 *   And a pass that is a pessimization on some code needs a way to be
 *   turned off by whoever is compiling, not only by whoever is writing
 *   the compiler.
 *
 * Returns 0 if the name is not a pass. `-Os` and -O levels set the
 * defaults; an explicit flag overrides whatever the level chose. */
int opt_set_pass(const char *name, int on);

/* Every pass name, for --help and for an error message that can list
 * them. Returns the count; names[] is static. */
int opt_pass_names(const char *const **names);

/* ---- the control-flow graph, for `embcc inspect cfg` (§18) ----
 *
 * The dump comes from the SAME builder the passes use (R1: one piece of
 * compiler knowledge, one implementation). A second CFG written for the
 * dump could disagree with the real one, and the case where it disagreed
 * would be exactly the case somebody was trying to debug.
 */
struct outbuf;
void opt_cfg_dump(struct outbuf *b, struct ir_func *fn);

#endif
