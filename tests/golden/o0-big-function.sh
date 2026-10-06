#!/bin/sh
# -O0 allocates registers only while a function is small enough for the
# dense liveness (ra_o0_too_big). Liveness is a bit set of every vreg at
# every instruction, so its cost is the square of the function's size: a
# generated function of 8000 statements took 5.5 s at -O0 on a Cortex-M
# once -O0 allocated, against 0.1 s before, and one of 84000 did not
# finish in ten minutes. Past the budget a function is compiled the way
# -O0 did before it allocated.
#
# Here, on Cortex-M (ARMv7-M, and ARMv6-M's own lowering), RV32 and AVR
# at -O0:
#   - a function of 6000 statements compiles to exactly the bytes
#     EMBCC_O0_NORA=1 gives (the old -O0), well inside a time limit;
#   - a small function does NOT: -O0 still allocates where it can.
set -u
echo "TEST-MARKER o0-big-function"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/o0-big-function
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
gen() {     # gen N FILE: a function of N statements on a volatile
    awk -v n="$1" 'BEGIN {
        print "volatile unsigned sink;"
        print "unsigned big(int x)\n{\n    sink = (unsigned)x;"
        for (i = 0; i < n; i++) print "    sink = sink * 3u + 1u;"
        print "    return sink;\n}" }' > "$2"
}
gen 6000 "$out/big.c"
gen 40 "$out/small.c"
# A compile that has not finished in 60 s has gone quadratic again.
limit() { perl -e 'alarm 60; exec @ARGV' "$@"; }
n=0
for t in thumbv7m-none-eabi thumbv6m-none-eabi riscv32-unknown-elf avr; do
    limit "$EMBCC" --target=$t -O0 -c "$out/big.c" -o "$out/$t-big.o" ||
        fail "$t: the 6000-statement function did not compile in 60 s"
    EMBCC_O0_NORA=1 "$EMBCC" --target=$t -O0 -c "$out/big.c" \
        -o "$out/$t-big-nora.o" || fail "$t: EMBCC_O0_NORA=1 big.c"
    cmp -s "$out/$t-big.o" "$out/$t-big-nora.o" ||
        fail "$t: the big function was allocated at -O0 (it is past the budget)"
    "$EMBCC" --target=$t -O0 -c "$out/small.c" -o "$out/$t-small.o" ||
        fail "$t: small.c"
    EMBCC_O0_NORA=1 "$EMBCC" --target=$t -O0 -c "$out/small.c" \
        -o "$out/$t-small-nora.o" || fail "$t: EMBCC_O0_NORA=1 small.c"
    if cmp -s "$out/$t-small.o" "$out/$t-small-nora.o"; then
        fail "$t: the small function was not allocated at -O0"
    fi
    n=$((n + 1))
done
echo "o0-big-function: on $n targets -O0 allocates a small function, and a 6000-statement one compiles as before -O0 allocated"
