#!/bin/sh
# C11 atomics and the __sync builtins on the Cortex-M: ldrex/strex retry
# loops (with the byte and halfword forms) between full dmb barriers.
#
# tests/golden/atomic-mcu.c checks what each operation computes, against the
# host, and then runs 200000 atomic updates in main while a SysTick handler
# updates the same variables -- so exclusive stores really do fail and the
# retry path really does run. Every total must equal iterations + ticks.
#
# QEMU runs with -icount: without it SysTick follows the host's clock and
# fired four times over the whole loop, which tests nothing. Counted
# instructions make the interrupt land thousands of times, and at the
# same places every run.
set -u
echo "TEST-MARKER thumb-atomic"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_THUMB:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
D=$EMBCC_ROOT/tests/golden
Q=$EMBCC_ROOT/tests/harness/qrun.sh
out=tests/golden/out/thumb-atomic
rm -rf "$out"; mkdir -p "$out"
cc -w -o "$out/host" "$D/atomic-mcu.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1)

run_on() {  # tag triple harness-dir harness-var qemu-machine cpu
    tag=$1 T=$2 H=$EMBCC_ROOT/tests/harness/$3 HV=$4 M=$5 CPU=$6
    "$QEMU" -machine help 2>/dev/null | grep -q "^$M " || {
        echo "SKIP $tag: this QEMU has no $M"; return 0; }
    B=$out/$tag; mkdir -p "$B"
    eval "$HV=\$PWD/\$B; export $HV"
    for f in boot io; do
        "$EMBCC" --target=$T -c "$H/$f.c" -o "$B/$f.o" || {
            echo "$tag: the harness does not compile"; exit 1; }
    done
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$D/atomic-mcu.c" -o "$B/a.o" || {
            echo "$tag $opt: does not compile"; exit 1; }
        sh "$H/link.sh" "$B/a.elf" "$B/a.o" > "$B/ln.log" 2>&1 || {
            echo "$tag $opt: does not link"; head -3 "$B/ln.log"; exit 1; }
        got=$(sh "$Q" 60 "$QEMU" -M "$M" -cpu "$CPU" -nographic \
                  -icount shift=2 -kernel "$B/a.elf" 2>/dev/null | head -1)
        [ "$got" = "$want" ] || {
            echo "$tag $opt: disagrees with the host"
            echo "  want: $want"
            echo "  got:  $got"
            echo "  (the second word onward: interleaved, then each total"
            echo "   == iterations + ticks)"; exit 1; }
    done
}
run_on m3 thumbv7m-none-eabi thumb EMBCC_THUMB_HARNESS lm3s6965evb cortex-m3
run_on m33 thumbv8m.main-none-eabi thumb-m33 EMBCC_M33_HARNESS mps2-an505 cortex-m33

# The barriers follow the order (thumb_atomic): an acquire exchange has
# one after and none before, a release one before, a relaxed none, a
# seq_cst both -- the fence-based mapping GCC uses on a Cortex-M. A lock's
# compare-exchange with a local `expected` takes pass_cxlocal's by-value
# form, which must carry the order too. And a leaf's loop keeps its
# status in a low register: no lr to push, `cmp r1, #0` at two bytes.
cat > "$out/ord.c" << 'E'
int x_acq(int *p) { return __atomic_exchange_n(p, 1, __ATOMIC_ACQUIRE); }
int x_rel(int *p) { return __atomic_exchange_n(p, 1, __ATOMIC_RELEASE); }
int x_rlx(int *p) { return __atomic_exchange_n(p, 1, __ATOMIC_RELAXED); }
int x_sc(int *p)  { return __atomic_exchange_n(p, 1, __ATOMIC_SEQ_CST); }
int a_acq(int *p, int v) { return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL); }
int c_acq(int *p)
{
    int e = 0;
    return __atomic_compare_exchange_n(p, &e, 1, 0, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}
E
"$EMBCC" --target=thumbv7m-none-eabi -Os -c "$out/ord.c" -o "$out/ord.o" ||
    { echo "FAIL: ord.c does not compile"; exit 1; }
if command -v llvm-objdump > /dev/null 2>&1; then
    llvm-objdump -d --no-show-raw-insn "$out/ord.o" > "$out/ord.dis"
    dmbs() {   # FUNCTION: "before after" -- barriers before and after its ldrex
        awk -v f="<$1>:" '$0 ~ f {p=1; next} /^[0-9a-f]+ <.*>:$/ {p=0}
             p && /dmb/ {if (seen) a++; else b++} p && /ldrex/ {seen=1}
             END {print b+0, a+0}' "$out/ord.dis"
    }
    for spec in "x_acq:0 1" "x_rel:1 0" "x_rlx:0 0" "x_sc:1 1" "a_acq:1 1" "c_acq:0 1"; do
        f=${spec%%:*}; want=${spec#*:}
        got=$(dmbs "$f")
        [ "$got" = "$want" ] || { echo "FAIL: $f has barriers '$got' (before after), wanted '$want'"; exit 1; }
    done
    awk '/<x_rlx>:/ {p=1; next} /^[0-9a-f]+ <.*>:$/ {p=0} p' "$out/ord.dis" > "$out/rlx.txt"
    grep -q "push" "$out/rlx.txt" && { cat "$out/rlx.txt"; echo "FAIL: a relaxed exchange leaf saves registers"; exit 1; }
    grep -qE "strex\s+r[0-7]," "$out/rlx.txt" || { cat "$out/rlx.txt"; echo "FAIL: the store's status is not in a low register"; exit 1; }
    grep -v "bx.lr" "$out/rlx.txt" | grep -q "lr" && { cat "$out/rlx.txt"; echo "FAIL: a leaf's exchange touches lr"; exit 1; }
    echo "the barriers follow the memory order, through pass_cxlocal too; a leaf's loop keeps its status low"
fi
echo "atomics compute what the host computes at 1, 2 and 4 bytes, and lose
no update when a SysTick handler races main for the same variables, on
ARMv7-M and ARMv8-M at -O0, -O1, -O2 and -Os"
