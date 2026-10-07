#!/bin/sh
# embrt: a stack bound proved from the compiler's frames (-fstack-usage),
# its call graph (-fcallgraph-info=su) and the objects' call relocations --
# refereed by running the programs. Every program of tests/exec that links
# without libc is built for the Cortex-M3 with lib/rt's helpers, bounded by
# embrt, and run on QEMU's lm3s6965evb under a startup that paints the free
# stack and reports how much of it main used. A bound below what ran is a
# wrong bound: the test fails on the first one. A program embrt cannot
# bound must say why (recursion, a dynamic frame, a missing frame).
set -u
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
EMBRT=${EMBRT:-./embrt}
[ -x "$EMBRT" ] || { echo "FAIL: $EMBRT is not built (make embrt)"; exit 1; }
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
echo "TEST-MARKER embrt"
out=tests/golden/out/embrt
rm -rf "$out"; mkdir -p "$out/rt" "$out/p"
fail() { echo "FAIL: $*"; exit 1; }
T=thumbv7m-none-eabi
F="-O2 -ffunction-sections -fstack-usage -fcallgraph-info=su"

# ---- the files the compiler writes ---------------------------------------
cat > "$out/cg.c" <<'EOF'
void g(void);
void (*fp)(void);
static void h(void) { g(); }
int v(int n) { volatile char b[n]; b[0] = 1; return b[0]; }
int main(void) { h(); fp(); g(); return v(3); }
EOF
"$EMBCC" --target=$T $F -c "$out/cg.c" -o "$out/cg.o" || fail "cg.c"
grep -q '^node: { title: "main" label: "main\\n[^"]*cg.c:5:5\\n[0-9]* bytes (static)" }' "$out/cg.ci" ||
    { cat "$out/cg.ci"; fail "-fcallgraph-info=su: no node for main with its frame"; }
grep -q 'edge: { sourcename: "main" targetname: "__indirect_call"' "$out/cg.ci" ||
    { cat "$out/cg.ci"; fail "-fcallgraph-info: the call through fp is not an indirect edge"; }
[ "$(grep -c 'edge: { sourcename: "main" targetname: "g"' "$out/cg.ci")" = 2 ] ||
    { cat "$out/cg.ci"; fail "-fcallgraph-info: main calls g twice (once through the inlined h)"; }
tab=$(printf '\t')
grep -q "cg.c:4:v${tab}[0-9]*${tab}dynamic" "$out/cg.su" ||
    { cat "$out/cg.su"; fail "-fstack-usage: a VLA's frame is not 'dynamic'"; }
grep -q "cg.c:5:main${tab}[0-9]*${tab}static" "$out/cg.su" ||
    { cat "$out/cg.su"; fail "-fstack-usage: main's frame is not 'static'"; }

# ---- lib/rt with frames: the helpers a backend calls ----------------------
rtobjs=
for c in lib/rt/*.c; do
    o="$out/rt/$(basename "$c" .c).o"
    "$EMBCC" --target=$T $F -Ilib/libc/include -c "$c" -o "$o" 2>/dev/null &&
        rtobjs="$rtobjs $o"
done
[ -n "$rtobjs" ] || fail "lib/rt does not compile for $T"
"$EMBCC" --target=$T -O2 -c tests/golden/embrt/paint.c -o "$out/paint.o" || fail paint.c
"$EMBCC" --target=$T -O2 -c tests/harness/thumb/io.c -o "$out/io.o" || fail io.c

if ! command -v "$QARM" >/dev/null 2>&1; then
    echo "SKIP: $QARM not found; the bounds were not run against the board"
    exit 0
fi

# ---- every program: the bound, then the run -------------------------------
checked=0 unbounded=0 skipped=0 tightest=0 loosest=0
for c in tests/exec/*.c; do
    n=$(basename "$c" .c)
    o="$out/p/$n.o"
    "$EMBCC" --target=$T $F -c "$c" -o "$o" 2>/dev/null || { skipped=$((skipped + 1)); continue; }
    # $rtobjs is a list: let the shell split it
    # shellcheck disable=SC2086
    "$EMBLD" -T tests/golden/ldscript/stm32.ld "$out/paint.o" "$out/io.o" "$o" \
        $rtobjs -o "$out/p/$n.elf" >/dev/null 2>&1 || { skipped=$((skipped + 1)); continue; }
    # shellcheck disable=SC2086
    "$EMBRT" "$o" $rtobjs --json > "$out/p/$n.json" 2> "$out/p/$n.err"
    st=$?
    if [ $st != 0 ]; then
        grep -q '"unbounded"' "$out/p/$n.json" ||
            { cat "$out/p/$n.err"; fail "$n: no bound and no reason given"; }
        unbounded=$((unbounded + 1))
        continue
    fi
    bound=$(sed -n 's/.*"name": "main", "kind": "entry", "bytes": \([0-9]*\).*/\1/p' "$out/p/$n.json")
    [ -n "$bound" ] || { cat "$out/p/$n.json"; fail "$n: no bound for main"; }
    sh tests/harness/qrun.sh 20 --until '==END==' "$QARM" -M lm3s6965evb \
        -cpu cortex-m3 -nographic -kernel "$out/p/$n.elf" > "$out/p/$n.run" 2>/dev/null
    used=$(sed -n 's/^STACK \([0-9]*\) *RET.*/\1/p' "$out/p/$n.run")
    if [ -z "$used" ]; then
        # a program that faults or never returns proves nothing either way
        skipped=$((skipped + 1))
        continue
    fi
    [ "$used" -le "$bound" ] ||
        { cat "$out/p/$n.json"; fail "$n: main used $used bytes of stack and embrt's bound is $bound"; }
    checked=$((checked + 1))
    slack=$((bound - used))
    [ $slack -gt $loosest ] && loosest=$slack
done
[ $checked -ge 50 ] || fail "only $checked programs were checked against the board"
echo "embrt: $checked programs ran within their bound (the loosest by $loosest bytes); $unbounded without one, each with its reason; $skipped need libc or did not run"

# ---- the reasons, and the budget ------------------------------------------
cat > "$out/r.c" <<'EOF'
int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
int vla(int n) { volatile char b[n]; b[0] = 1; return b[0]; }
int ext(int);
int via(int n) { return ext(n); }
int (*const tab[1])(int) = { fact };
int ind(int k, int x) { return tab[k & 0](x); }
int ok(int x) { return x + 1; }
EOF
"$EMBCC" --target=$T $F -c "$out/r.c" -o "$out/r.o" || fail r.c
"$EMBRT" "$out/r.o" --entry fact > "$out/r1.txt" 2>&1 && fail "recursion is bounded"
grep -q 'recursion' "$out/r1.txt" || { cat "$out/r1.txt"; fail "recursion is not named"; }
"$EMBRT" "$out/r.o" --entry vla > "$out/r2.txt" 2>&1 && fail "a VLA frame is bounded"
grep -q 'grows at run time' "$out/r2.txt" || { cat "$out/r2.txt"; fail "the dynamic frame is not named"; }
"$EMBRT" "$out/r.o" --entry via > "$out/r3.txt" 2>&1 && fail "a call to an unknown function is bounded"
grep -q 'no object defines it' "$out/r3.txt" || { cat "$out/r3.txt"; fail "the missing callee is not named"; }
"$EMBRT" "$out/r.o" --entry ind > "$out/r4.txt" 2>&1 && fail "an indirect call reaching recursion is bounded"
grep -q 'recursion' "$out/r4.txt" || { cat "$out/r4.txt"; fail "the indirect call's target is not followed"; }
ok=$("$EMBRT" "$out/r.o" --entry ok --json | sed -n 's/.*"bytes": \([0-9]*\).*/\1/p')
"$EMBRT" "$out/r.o" --entry ok --max-stack "$ok" >/dev/null 2>&1 || fail "a stack exactly at --max-stack is refused"
"$EMBRT" "$out/r.o" --entry ok --max-stack $((ok - 1)) >/dev/null 2>&1 && fail "a stack over --max-stack passes"

# interrupts: the deepest handler and its hardware frame on top of the
# entry's worst case, or every handler at once with --nested
cat > "$out/i.c" <<'EOF'
int deep(int x) { volatile int b[30]; b[x & 31] = x; return b[3]; }
int shallow(int x) { return x + 1; }
void isr_a(void) { (void)deep(1); }
void isr_b(void) { (void)shallow(2); }
EOF
"$EMBCC" --target=$T $F -c "$out/i.c" -o "$out/i.o" || fail i.c
bound_of() {   # bound_of NAME OBJ...
    nm=$1; shift
    "$EMBRT" "$@" --entry "$nm" --json |
        sed -n "s/.*\"name\": \"$nm\", \"kind\": \"entry\", \"bytes\": \([0-9]*\).*/\1/p"
}
b1=$(bound_of ok "$out/r.o"); ia=$(bound_of isr_a "$out/i.o"); ib=$(bound_of isr_b "$out/i.o")
[ -n "$b1" ] && [ -n "$ia" ] && [ -n "$ib" ] || fail "no bounds for the interrupt test"
deepest=$((ia > ib ? ia : ib))
t1=$("$EMBRT" "$out/r.o" "$out/i.o" --entry ok --isr isr_a --isr isr_b --isr-frame 32 --json | sed -n 's/.*"total": \([0-9]*\).*/\1/p')
[ "$t1" = $((b1 + deepest + 32)) ] || fail "with interrupts: $t1, want $b1 + $deepest + 32"
t2=$("$EMBRT" "$out/r.o" "$out/i.o" --entry ok --isr isr_a --isr isr_b --isr-frame 32 --nested --json | sed -n 's/.*"total": \([0-9]*\).*/\1/p')
[ "$t2" = $((b1 + ia + ib + 64)) ] || fail "nested interrupts: $t2, want $b1 + $ia + $ib + 64"

# a file-scope static is its own file's: two of one name do not mix
printf 'static int h(int x) { volatile int b[40]; b[x & 39] = x; return b[1]; }\nint fa(int x) { return h(x) + 1; }\n' > "$out/sa.c"
printf 'static int h(int x) { return x * 3; }\nint fb(int x) { return h(x) + 2; }\n' > "$out/sb.c"
for f in sa sb; do
    "$EMBCC" --target=$T -O0 -ffunction-sections -fstack-usage -fcallgraph-info=su \
        -c "$out/$f.c" -o "$out/$f.o" || fail "$f.c"
done
fbb=$(bound_of fb "$out/sa.o" "$out/sb.o")
want=$(awk -F'\t' '/:fb\t|:h\t/ { s += $2 } END { print s }' "$out/sb.su")
[ "$fbb" = "$want" ] || fail "fb's bound is $fbb; its own file's h makes it $want"

echo "embrt: recursion, dynamic frames, unknown callees and indirect calls named; interrupts; statics per file; --max-stack"
