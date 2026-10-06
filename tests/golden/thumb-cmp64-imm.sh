#!/bin/sh
# 64-bit comparisons on Thumb, with a constant and without.
#
# strtol's range checks (`v > LONG_MAX`, v an unsigned long long) built
# 0x7fffffff in r2:r3, copied it and v into r9-r12 and pushed r9-r11 to
# compare them: 170 bytes where clang has 76. A constant whose halves are
# modified immediates now stays in the instruction -- `subs; sbcs` of
# x - (K + 1) for `x > K`, `rsbs; mvn; adcs` of K - x when K + 1 does
# not encode, `cmp lo; it eq; cmpeq hi` for equality -- and two values in
# registers are compared where they live, the swapped `>` included.
#
# Shapes first, then what they compute: embedded-cmp64.c on the
# Cortex-M3 board at every level, against the same program on the host.
set -u
echo "TEST-MARKER thumb-cmp64-imm"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/thumb-cmp64-imm
rm -rf "$out"; mkdir -p "$out"

cat > "$out/c.c" <<'EOT'
typedef unsigned long long u64;
int gtmax(u64 v) { return v > 0x7fffffffULL; }       /* subs #0x80000000 */
int gtmin(u64 v) { return v > 0x80000000ULL; }       /* rsbs #0x80000000 */
int sgt(long long v) { if (v > 1000) return 3; return 5; }
int eqk(long long v) { return v == 0x100000000LL; }
int lt(long long a, long long b) { return a < b; }
int gt(long long a, long long b) { if (a > b) return 7; return 9; }
int eq(long long a, long long b) { if (a == b) return 7; return 9; }
EOT
"$EMBCC" --target=thumbv7em-none-eabi -O2 -c "$out/c.c" -o "$out/c.o" 2> "$out/cc.log" || {
    echo "FAIL: could not compile:"; cat "$out/cc.log"; exit 1; }
"$OD" -d --no-show-raw-insn --triple=thumbv7em "$out/c.o" > "$out/c.dis"
body() {                        # body FUNC -> the function's instructions
    awk -v f="<$1>:" '$2 == f { on = 1; next } /^[0-9a-f]+ <.*>:$/ { on = 0 }
                      on && /^ *[0-9a-f]+:/' "$out/c.dis"
}
n() { body "$1" | grep -cE "$2" || true; }
fail() { echo "FAIL: $1:"; body "$2"; exit 1; }

for f in gtmax gtmin sgt eqk lt gt eq; do
    [ "$(n $f 'push|movw|movt|r9|r10|r11')" = 0 ] ||
        fail "$f should build no constant and push nothing" $f
done
[ "$(n gtmax 'subs')" = 1 ] && [ "$(n gtmax 'sbcs')" = 1 ] ||
    fail "gtmax should be a subs and an sbcs of K + 1's halves" gtmax
[ "$(n gtmin 'rsbs')" = 1 ] && [ "$(n gtmin 'mvn')" = 1 ] &&
    [ "$(n gtmin 'adcs')" = 1 ] ||
    fail "gtmin should be rsbs, mvn, adcs of K - x" gtmin
[ "$(n sgt 'rsbs')" = 1 ] || fail "sgt should be K - x: 1001 does not encode" sgt
[ "$(n eqk 'cmpeq')" = 1 ] || fail "eqk should be cmp; it eq; cmpeq" eqk
[ "$(n eq 'cmpeq')" = 1 ] || fail "eq should be cmp; it eq; cmpeq" eq
echo "x > 0x7fffffff, x > 0x80000000, x > 1000, x == 1 << 32: K in the instructions"
echo "a < b, a > b, a == b: compared where they live, nothing pushed"

# What they compute.
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP (exec): $QEMU not found (set EMBCC_QEMU_ARM)"; exit 0; }
T=thumbv7m-none-eabi
H=tests/harness/thumb
export EMBCC_THUMB_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
        echo "FAIL: the harness does not compile for $T"; exit 1; }
done
cc -std=c99 -w -o "$out/host" tests/golden/embedded-cmp64.c "$H/hostio.c" || {
    echo "FAIL: the program does not compile for the host"; exit 1; }
"$out/host" > "$out/ref.txt" || { echo "FAIL: the host run failed"; exit 1; }
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c tests/golden/embedded-cmp64.c \
             -o "$out/p$opt.o" || {
        echo "FAIL: $opt: the program does not compile"; exit 1; }
    sh "$H/link.sh" "$out/p$opt.elf" "$out/p$opt.o" || {
        echo "FAIL: $opt: embld could not link the image"; exit 1; }
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" --until '==END==' \
        "$QEMU" -M lm3s6965evb -cpu cortex-m3 -nographic \
        -kernel "$out/p$opt.elf" > "$out/p$opt.txt" 2>/dev/null
    grep -q '==END==' "$out/p$opt.txt" || {
        echo "FAIL: $opt: the image did not reach the end of main:"
        sed -n '1,10p' "$out/p$opt.txt"; exit 1; }
    if ! diff -u "$out/ref.txt" "$out/p$opt.txt" > "$out/p$opt.diff"; then
        echo "FAIL: $opt: the comparisons do not agree with the host:"
        head -20 "$out/p$opt.diff"; exit 1
    fi
done
echo "embedded-cmp64: 16 constants, every predicate, agree with the host at four levels"
