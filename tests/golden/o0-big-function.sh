#!/bin/sh
# -O0 allocates registers in a function of up to 100000 IR instructions
# (ra_o0_too_big). Liveness was once a bit set of every vreg at every
# instruction and the interference graph a bit matrix, so allocating cost
# the square of a function's size: a generated function of 8000 statements
# took 5.5 s at -O0 on a Cortex-M, one of 84000 did not finish in ten
# minutes, and the limit had to stop at about 2000 statements. Both are
# linear now, and the limit only caps a cost that is merely large -- AVR
# generates each function several times. Past it a function is compiled
# the way -O0 did before it allocated.
#
# Here, on Cortex-M (ARMv7-M, and ARMv6-M's own lowering), RV32 and AVR at -O0, each compile well inside a time
# limit:
#   - a function of 6000 statements (48000 to 60000 instructions) is
#     allocated: it does NOT compile to the bytes EMBCC_O0_NORA=1 gives;
#   - one of 16000 (128000 to 160000) is past the limit and does;
#   - a small function is allocated.
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
gen 6000 "$out/mid.c"
gen 16000 "$out/big.c"
gen 40 "$out/small.c"
# A compile that has not finished in 60 s has gone quadratic again.
limit() { perl -e 'alarm 60; exec @ARGV' "$@"; }
n=0
for t in thumbv7m-none-eabi thumbv6m-none-eabi riscv32-unknown-elf avr; do
    for f in small mid big; do
        limit "$EMBCC" --target=$t -O0 -c "$out/$f.c" -o "$out/$t-$f.o" ||
            fail "$t: $f.c did not compile at -O0 in 60 s"
        # through env: an assignment before a shell FUNCTION may stay set
        # after it, in some shells, and then every compile is NORA
        limit env EMBCC_O0_NORA=1 "$EMBCC" --target=$t -O0 -c "$out/$f.c" \
            -o "$out/$t-$f-nora.o" || fail "$t: EMBCC_O0_NORA=1 $f.c"
    done
    cmp -s "$out/$t-big.o" "$out/$t-big-nora.o" ||
        fail "$t: the 16000-statement function was allocated at -O0 (it is past the limit)"
    if cmp -s "$out/$t-mid.o" "$out/$t-mid-nora.o"; then
        fail "$t: the 6000-statement function was not allocated at -O0"
    fi
    if cmp -s "$out/$t-small.o" "$out/$t-small-nora.o"; then
        fail "$t: the small function was not allocated at -O0"
    fi
    n=$((n + 1))
done
echo "o0-big-function: on $n targets -O0 allocates a small function and a 6000-statement one, and a 16000-statement one compiles as before -O0 allocated"
