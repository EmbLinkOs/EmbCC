#!/bin/sh
# Partial redundancy elimination: the case the two CSEs cannot reach.
#
# `if (c) use(E); ... E` computes E twice when c was true and once when
# it was false. Neither CSE helps -- local value numbering sees two
# blocks, and global CSE wants the earlier computation to DOMINATE the
# later one, which it does not. PRE inserts E on the path that lacked
# it, which makes the second copy a move.
#
# Two things are checked here and they are different claims. The IR
# checks say the rewrite FIRED, and are the only reason the multiply
# arm of the cost model is not dead code -- no multiply insertion
# happens anywhere in lib/libc or lib/libcxx. The run checks say it was
# RIGHT, which matters more: this pass creates a vreg written in several
# predecessors and reads a value in a block none of its definitions
# dominates, and either of those getting the merge wrong gives a wrong
# answer rather than a slow one.
set -eu
echo "TEST-MARKER pre"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/pre
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || { echo "skipped: the runner here is x86-64"; exit 0; }

ir() {
    EMBCC_VERIFY=1 "$EMBCC" inspect ir --target=x86_64-linux-gnu -O2 "$1" \
        > "$out/ir.txt" 2> "$out/err.txt" || {
        echo "FAIL: could not dump IR for $1:"; cat "$out/err.txt"; exit 1; }
}

# ---- 1. the insertion --------------------------------------------------
cat > "$out/ins.c" <<'EOF'
void sink(long);
long f(long a, long b, int c)
{
    if (c) sink(a * b);
    return a * b;
}
EOF
ir "$out/ins.c"
nmul=$(grep -cE '^  %[0-9]+ = mul' "$out/ir.txt" || true)
[ "$nmul" = 2 ] || {
    echo "FAIL: $nmul multiplies where two would do -- one per path:"
    cat "$out/ir.txt"; exit 1; }
# the merge block must no longer compute anything: it reads the name the
# two paths agreed on
grep -qE '^  %[0-9]+ = mov' "$out/ir.txt" || {
    echo "FAIL: nothing became a move, so nothing was merged:"
    cat "$out/ir.txt"; exit 1; }
n=$("$EMBCC" --target=x86_64-linux-gnu -O2 -fremarks -c "$out/ins.c" \
      -o /dev/null 2>&1 | sed -n 's/.*, \([0-9]*\) partial redundancies.*/\1/p' |
    awk '{s+=$1} END {print s+0}')
[ "$n" -ge 1 ] || { echo "FAIL: the pass reports no rewrite"; exit 1; }
echo "a multiply on one path and the merge below it become one per path"

# ---- 2. the critical edge got split, not straddled ---------------------
#
# `if (c)` branches straight to the merge, so there is nowhere to put
# the insertion that is not also on the other path. Putting it before
# the branch would compute it for BOTH successors, which is work
# invented on a path that never wanted it. The new block is the proof
# that did not happen: the multiply the insertion added must sit under a
# label of its own, not above the branch.
awk '/brz|brnz/ { br = NR } /= mul/ { if (br && NR > br) after = 1 }
     END { exit(after ? 0 : 1) }' "$out/ir.txt" || {
    echo "FAIL: no multiply below the branch -- the insertion was placed"
    echo "      before it, where both successors pay for it:"
    cat "$out/ir.txt"; exit 1; }
nlab=$(grep -cE '^L[0-9]+:' "$out/ir.txt" || true)
[ "$nlab" -ge 2 ] || {
    echo "FAIL: only $nlab labels -- the critical edge was not split:"
    cat "$out/ir.txt"; exit 1; }
echo "the critical edge is split, so the insertion is on one edge only"

# ---- 3. reuse where no definition dominates ----------------------------
#
# After the merge, the value is held under one name on every path but is
# dominated by none of its definitions. Global CSE asks about dominance
# and so cannot reuse it; this pass asks whether every path has passed a
# definition, which is the weaker and here the true condition.
cat > "$out/reuse.c" <<'EOF'
void sink(long);
long f(long a, long b, int c)
{
    if (c) sink(a * b);
    sink(a * b);
    return a * b;
}
EOF
ir "$out/reuse.c"
nmul=$(grep -cE '^  %[0-9]+ = mul' "$out/ir.txt" || true)
[ "$nmul" -le 2 ] || {
    echo "FAIL: $nmul multiplies -- the merged value was not reused"
    echo "      below the block it was merged in:"; cat "$out/ir.txt"; exit 1; }
echo "the merged value is reused downstream, where nothing dominates it"

# ---- 4. and all of it still computes the same thing --------------------
#
# The checks above say a rewrite fired and where it landed. This says
# the merge is right: every path through every shape, at three
# optimization levels, against gcc.
cat > "$out/run.c" <<'EOF'
#include <stdio.h>

static long sunk;
static void sink(long v) { sunk = sunk * 31 + v; }

/* the shape at the top of the pass: one path computes it, both use it */
static long one(long a, long b, int c)
{
    if (c) sink(a * b);
    return a * b;
}

/* two arms, neither dominating the merge */
static long two(long a, long b, int c)
{
    if (c) sink(a * b); else sink(a + b);
    return a * b;
}

/* a use below the merge, which only availability can reuse */
static long below(long a, long b, int c)
{
    if (c) sink(a * b);
    sink(a * b);
    return a * b;
}

/* the insertion must not escape a loop: it goes at the end of a
 * predecessor, so it runs exactly as often as the block it feeds */
static long looped(long a, long b, int n)
{
    long t = 0;
    for (int i = 0; i < n; i++) {
        if (i & 1) sink(a * b);
        t += a * b;
    }
    return t;
}

/* nested, so a merge is itself a predecessor of another merge */
static long nested(long a, long b, int c, int e)
{
    if (c) { if (e) sink(a * b); else sink(b - a); }
    else sink(a + b);
    return a * b;
}

/* a compare, to exercise a key whose result feeds a branch */
static long cmped(long a, long b, int c)
{
    if (c) sink(a < b);
    return (a < b) ? a : b;
}

int main(void)
{
    static const long v[] = { 0, 1, -1, 2, -3, 1000, -1000,
                              0x7fffffffL, -0x7fffffffL - 1 };
    unsigned long h = 0;
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
        for (unsigned j = 0; j < sizeof v / sizeof v[0]; j++)
            for (int c = 0; c < 2; c++) {
                long a = v[i], b = v[j];
                sunk = 0;
                h = h*1000003u + (unsigned long)one(a, b, c);
                h = h*1000003u + (unsigned long)two(a, b, c);
                h = h*1000003u + (unsigned long)below(a, b, c);
                h = h*1000003u + (unsigned long)looped(a, b, c ? 7 : 0);
                h = h*1000003u + (unsigned long)nested(a, b, c, (int)i & 1);
                h = h*1000003u + (unsigned long)cmped(a, b, c);
                h = h*1000003u + (unsigned long)sunk;
            }
    printf("%lu", h);
    putchar(10);
    return 42;
}
EOF

LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
if [ -f "$LIBDIR/libc.a" ] &&
   "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1
then
    prev=
    for O in 0 1 2; do
        EMBCC_VERIFY=1 "$EMBCC" --target=x86_64-linux-gnu "-O$O" "$out/run.c" \
            -o "$out/r" 2> "$out/cc.log" || {
            echo "FAIL: build at -O$O:"; cat "$out/cc.log"; exit 1; }
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/r" \
            > "$out/r.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: exited $rc at -O$O:"
                            cat "$out/r.txt"; exit 1; }
        ans=$(cat "$out/r.txt")
        [ -z "$prev" ] || [ "$prev" = "$ans" ] || {
            echo "FAIL: answers $ans at -O$O where a lower level said $prev."
            echo "      A merged value took the wrong path's answer."
            exit 1; }
        prev=$ans
    done
    echo "the same answer at -O0, -O1 and -O2 ($prev)"

    GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
    if command -v "$GCC" > /dev/null 2>&1; then
        "$GCC" -O2 -c "$out/run.c" -I"$EMBCC_ROOT/lib/libc/include" \
            -o "$out/g.o" 2> /dev/null || {
            echo "FAIL: gcc will not build the case"; exit 1; }
        "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
            "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
            "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/g" \
            > "$out/g.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc"; exit 1; }
        [ "$(cat "$out/g.txt")" = "$prev" ] || {
            echo "FAIL: gcc answers $(cat "$out/g.txt"), embcc answers $prev"
            exit 1; }
        echo "and the same answer as gcc"
    fi
else
    echo "(not run: needs a kernel and a libc for tests/harness/linux)"
fi
