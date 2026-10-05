#!/bin/sh
# -O2 compile time grows with a function's size, not its square.
#
# A generated function of N plain statements used to compile in time
# quadratic in N: 8000 statements took 30 s on a Cortex-M at -O2, against
# well under a second for clang. Copy propagation walked the function once
# per move, value numbering searched its table linearly, the CFG passes
# scanned blocks against blocks, liveness was a bit set of every vreg at
# every instruction and the allocator's graph a bit matrix of its nodes.
# Each of those is linear (or n log n) now, so these compile in a second or
# two; one that has not finished in 120 s has gone quadratic again.
#
# Shapes that each once hit a different one of them -- a long run of
# statements, a big switch, a run of if statements, a function storing
# every field of a big struct (a dense interference graph) -- on Cortex-M,
# RV32, AVR and x86-64. Only that each compiles in time is checked:
# tests/exec and the corpus comparisons say what the code is.
set -u
echo "TEST-MARKER o2-big-function"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/o2-big-function
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

awk 'BEGIN {
    print "volatile unsigned sink;"
    print "unsigned big(int x)\n{\n    sink = (unsigned)x;"
    for (i = 0; i < 20000; i++) print "    sink = sink * 3u + 1u;"
    print "    return sink;\n}" }' > "$out/straight.c"
awk 'BEGIN {
    print "volatile unsigned sink;"
    print "unsigned sw(unsigned x, unsigned y)\n{\n    switch (x) {"
    for (i = 0; i < 3000; i++)
        printf "    case %d: sink = y * %du + %d; break;\n", i * 3, i, i
    print "    default: sink = 0;\n    }\n    return sink;\n}" }' > "$out/switch.c"
awk 'BEGIN {
    print "volatile unsigned sink;"
    print "unsigned ifs(unsigned x, unsigned y)\n{\n    unsigned r = y;"
    for (i = 0; i < 6000; i++)
        printf "    if (y & %du) r ^= x + %d;\n", (i % 31) + 1, i
    print "    sink = r;\n    return r;\n}" }' > "$out/ifs.c"
awk 'BEGIN {
    print "struct big {"
    for (i = 0; i < 3000; i++) printf "    int f%d;\n", i
    print "};\nvoid init(struct big *s, int x)\n{"
    for (i = 0; i < 3000; i++) printf "    s->f%d = x * %d + %d;\n", i, i, i % 17
    print "}" }' > "$out/struct.c"

# A compile that has not finished in 120 s has gone quadratic again.
limit() { perl -e 'alarm 120; exec @ARGV' "$@"; }
n=0
for t in thumbv7m-none-eabi riscv32-unknown-elf avr x86_64-elf; do
    for s in straight switch ifs struct; do
        limit "$EMBCC" --target=$t -O2 -c "$out/$s.c" -o "$out/$t-$s.o" ||
            fail "$t: $s.c did not compile at -O2 in 120 s"
        [ -s "$out/$t-$s.o" ] || fail "$t: $s.c gave no object"
        n=$((n + 1))
    done
done
echo "o2-big-function: $n big functions compiled at -O2, each inside the time limit"
