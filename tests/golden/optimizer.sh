#!/bin/sh
# The optimizer at -O1: the IR passes (src/opt) AND codegen's RAX residency
# cache that elides redundant reloads. Three properties, each load-bearing:
#
#  1. -O0 is byte-for-byte the no-flag output. The self-host fixed point
#     rests on this, so it is asserted here directly, not just assumed.
#  2. -O1 preserves semantics. Every tests/exec program is recompiled at
#     -O1 and must still produce its `// expect-exit` value — the same
#     differential net the -O0 suite is, now over the optimized path.
#  3. -O1 actually optimizes: a program full of foldable/dead/copy work,
#     plus store/reload traffic, compiles to a strictly smaller object.
set -u
echo "TEST-MARKER optimizer"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/optimizer
rm -rf "$out"; mkdir -p "$out"

# 1. -O0 == no flag, byte for byte.
prog="$out/id.c"
cat > "$prog" <<'EOF'
int f(int x){ int a = 2 + 3; int b = x * 1; int c = a + 0; return b + c + x; }
int main(void){ return f(20); }
EOF
"$EMBCC" -c "$prog" -o "$out/none.o"    || { echo "compile (no flag) failed"; exit 1; }
"$EMBCC" -c -O0 "$prog" -o "$out/o0.o"  || { echo "compile -O0 failed"; exit 1; }
cmp -s "$out/none.o" "$out/o0.o" || { echo "-O0 differs from no-flag output"; exit 1; }
echo "-O0 is byte-identical to unoptimized output"

# 3. -O1 optimizes: fold (2+3), identity (x*1, a+0), and the dead temps they
#    leave must shrink the object.
"$EMBCC" -c -O1 "$prog" -o "$out/o1.o"  || { echo "compile -O1 failed"; exit 1; }
s0=$(wc -c < "$out/o0.o" | tr -d " "); s1=$(wc -c < "$out/o1.o" | tr -d " ")
[ "$s1" -lt "$s0" ] || { echo "-O1 did not shrink the object ($s0 -> $s1)"; exit 1; }
echo "-O1 folds/eliminates: object $s0 -> $s1 bytes"

# 2. -O1 preserves the semantics of every exec test.
n=0
for c in tests/exec/*.c; do
    [ -e "$c" ] || continue
    exp=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" | head -1)
    [ -z "$exp" ] && continue
    name=$(basename "$c" .c)
    pinned_elsewhere "$c" && continue
    o="$out/$name.o"; e="$out/$name"
    "$EMBCC" --target="$TARGET" -c -O1 "$c" -o "$o" || {
        echo "-O1 failed to compile $c"; exit 1; }
    t_link "$e" "$o" || { echo "link failed for $c"; exit 1; }
    t_run "$e" >/dev/null 2>&1; got=$?
    [ "$got" -eq "$exp" ] || { echo "-O1 $name: got $got, want $exp"; exit 1; }
    n=$((n + 1))
done
echo "-O1 preserved semantics across all $n exec programs"

# 4. Dead code that is a CYCLE.
#
# An accumulator nothing reads is two instructions that feed each other
# -- `next = acc * 31 + i` and `acc = next` -- so each has a use, a use
# count never reaches zero for either, and dead-code elimination that
# counts uses leaves the pair there for the life of the function.
# Liveness is therefore computed by MARKING what is reachable from an
# effect, and a cycle no effect reaches is marked by nothing.
#
# The loop itself stays: its counter feeds the test, and deleting a loop
# needs a termination argument this compiler does not have. What must go
# is the multiply, which is the whole of the dead work.
cat > "$out/cycle.c" <<'EOF'
int f(int n)
{
    int dead = 0;
    for (int i = 0; i < n; i++)
        dead = dead * 31 + i;      /* nothing ever reads `dead` */
    return n;
}
EOF
"$EMBCC" inspect ir --target="$TARGET" -O2 "$out/cycle.c" > "$out/cycle.ir" \
    2>/dev/null || { echo "FAIL: could not dump the dead-cycle IR"; exit 1; }
if grep -q 'mul' "$out/cycle.ir"; then
    echo "FAIL: the multiply of an accumulator nothing reads survives."
    echo "      It feeds only the copy that feeds it back, so a use"
    echo "      count cannot remove it:"
    cat "$out/cycle.ir"; exit 1
fi
echo "an accumulator that feeds only itself is removed, cycle and all"

# 5. Blocks that are not blocks, and branches that are not branches.
#
# A label nothing can jump to is not a block boundary, it is only
# pretending to be one -- and every block-local pass stops at it. Value
# numbering, copy propagation, store forwarding and dead-store
# elimination all reason between labels, so one no branch names splits a
# straight line in two for nothing. The loop passes leave these behind
# by the handful.
#
# And a second branch on a condition the one above it already decided
# cannot fire: control reaches it only by falling out of the first, and
# with no label between them nothing can jump in to make that untrue.
cat > "$out/cfg.c" <<'EOF'
int g[64];
long sum(long n)   { long s = 0; for (long i = 0; i < n; i++) s += i & 7; return s; }
long walk(int n)   { long s = 0; for (int i = 0; i < n; i++) s += g[i]; return s; }
int  pick(int a, int b) { int r; if (a > b) r = a - b; else r = b - a; return r * 2; }
EOF
"$EMBCC" inspect ir --target="$TARGET" -O2 "$out/cfg.c" > "$out/cfg.ir" \
    2>/dev/null || { echo "FAIL: could not dump the CFG-cleanup IR"; exit 1; }
orphan=$(awk '
    /^  *(jmp|brz|brnz)/ { for (i = 1; i <= NF; i++)
                               if ($i ~ /^L[0-9]+$/) used[$i] = 1 }
    /^L[0-9]+:/ { l = $1; sub(":", "", l); defd[l] = 1 }
    END { for (l in defd) if (!(l in used)) print l }
' "$out/cfg.ir")
[ -z "$orphan" ] || {
    echo "FAIL: label(s) $orphan are defined and never jumped to, so they"
    echo "      split a straight line into two blocks for nothing:"
    cat "$out/cfg.ir"; exit 1; }
echo "every label left is one something can jump to"

twice=$(awk '
    /^L[0-9]+:/ { prev = ""; next }
    /^  *br(z|nz)/ { c = $2; sub(/^[^%]*/, "", c); sub(/[ \t].*/, "", c)
                     if (c != "" && c == prev) { print NR; exit }
                     prev = c; next }
    { prev = prev }
' "$out/cfg.ir")
[ -z "$twice" ] || {
    echo "FAIL: two branches in a row test the same value (IR line $twice)."
    echo "      The second cannot fire -- nothing can jump between them:"
    cat "$out/cfg.ir"; exit 1; }
echo "no branch asks a question the branch above it has answered"

# 6. And a computed goto does not turn the optimizer off.
#
# It used to: an indirect jump reaches any address-taken label, those
# edges were not in the CFG, so the blocks those labels open looked
# unreachable and the passes that delete unreachable code deleted the
# program. The edges are modelled now. What still stands aside is the
# passes that put an instruction ON AN EDGE -- there is no block between
# `goto *p` and its target to put one in -- and everything else runs.
cat > "$out/cgoto.c" <<'EOF'
int dispatch(int start, int n)
{
    void *ops[2] = { &&STEP, &&DONE };
    int acc = 0, i = 0;
    goto *ops[start];
STEP:  acc += 2 * 3;          /* foldable, inside a computed-goto function */
       i++;
       goto *ops[i < n ? 0 : 1];
DONE:  return acc;
}
EOF
"$EMBCC" inspect ir --target="$TARGET" -O2 "$out/cgoto.c" > "$out/cgoto.ir"     2>/dev/null || { echo "FAIL: could not dump the computed-goto IR"; exit 1; }
grep -q 'igoto' "$out/cgoto.ir" || {
    echo "FAIL: no indirect jump in the IR, so this is not testing it:"
    cat "$out/cgoto.ir"; exit 1; }
if grep -qE 'mul' "$out/cgoto.ir"; then
    echo "FAIL: '2 * 3' survives as a multiply, so the optimizer is still"
    echo "      switched off for a function with a computed goto:"
    cat "$out/cgoto.ir"; exit 1
fi
echo "a function with a computed goto is optimized, not skipped"

echo "optimizer acceptance passed"
