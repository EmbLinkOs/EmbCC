#!/bin/sh
# ra_parallel_move, checked by SIMULATION over every small shape.
#
# Three sites in a register-allocating backend need "put these values in
# these registers, all at once": a call's argument setup, a prologue's
# parameter placement, and an indirect call's target. The first attempt
# at them (the parked branch thumb-regalloc-wip) open-coded the ordering
# at each site and got three separate bugs out of it, which is why the
# routine exists once and is proven before anything depends on it.
#
# tools/pmovecheck does not inspect the sequence: it EXECUTES it against
# a model register file and requires every destination to hold what its
# source held BEFORE the move began. That is the property, stated
# directly. Exhaustive over every mapping of up to five destinations
# drawn from five registers, so chains, cycles, swaps and fan-outs are
# covered by construction rather than by a list someone thought of.
set -u
echo "TEST-MARKER parallel-move"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/parallel-move
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/pmovecheck" \
   tools/pmovecheck/pmovecheck.c src/arch/regalloc.c src/driver/util.c \
   src/driver/diag.c src/driver/remark.c src/platform/platform_posix.c \
   src/ir/irprint.c src/sema/type.c src/sema/ldfloat.c src/arch/target.c || {
    echo "pmovecheck did not build"; exit 1; }

"$out/pmovecheck" || exit 1
