#!/bin/sh
# Conditional branches further than B<c>.W's +-1 MB. A function that big
# is rare, but -O0 makes one out of a few thousand lines of ordinary code,
# and the conditional form's offset was patched with no range check: past
# 1 MB its top bits were dropped and the branch went somewhere else,
# assembling cleanly. Now the backend sees on its first pass that a
# conditional branch cannot reach, and makes the function again with every
# conditional jump to a label as `b<!c> .+n; b.w label` (+-16 MB). Here a
# forward branch over a then-arm of more than 1 MB, and a loop whose
# backward branch spans more than 1 MB, run on the Cortex-M4 and compared
# with the host. The function's size is checked, so the test cannot pass
# by having become small.
set -u
echo "TEST-MARKER thumb-far"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
"$QEMU" -M help 2>/dev/null | grep -q mps2-an386 || {
    echo "SKIP: this QEMU has no mps2-an386"; exit 0; }

T=thumbv7em-none-eabi
H=$EMBCC_ROOT/tests/harness/thumb-m4f
out=tests/golden/out/thumb-far
rm -rf "$out"; mkdir -p "$out"
EMBCC_THUMB_HARNESS=$PWD/$out; export EMBCC_THUMB_HARNESS
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
        echo "the M4 harness does not compile"; exit 1; }
done

cat > "$out/far.c" <<'CEOF'
void writec(int c); void puts_(const char *s);
volatile unsigned sink;
static void hx(unsigned v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
#define S1   sink = sink * 3u + 1u;
#define S10  S1 S1 S1 S1 S1 S1 S1 S1 S1 S1
#define S100 S10 S10 S10 S10 S10 S10 S10 S10 S10 S10
#define S1K  S100 S100 S100 S100 S100 S100 S100 S100 S100 S100
#define S6K  S1K S1K S1K S1K S1K S1K
__attribute__((noinline)) unsigned far(int x, int k)
{
    sink = (unsigned)x;
    if (x > 3) {                     /* forward, over more than 1 MB */
        S6K S6K
    } else {
        sink = sink + 7u;
    }
    do {                             /* backward, over more than 1 MB */
        S6K S6K
    } while (--k > 0);
    return sink;
}
int main(void)
{
    hx(far(5, 2)); hx(far(1, 1)); hx(far(9, 3));
    puts_("\n==END==\n");
    return 0;
}
CEOF
cc -w -o "$out/host" "$out/far.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1)
[ -n "$want" ] || { echo "the host reference printed nothing"; exit 1; }

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
NM=${EMBCC_LLVM_NM:-llvm-nm}
"$EMBCC" --target=$T -O0 -c "$out/far.c" -o "$out/far.o" || {
    echo "-O0: does not compile"; exit 1; }
if command -v "$NM" >/dev/null 2>&1; then
    sz=$("$NM" -S "$out/far.o" | awk '$4 == "far" { print $2 }')
    [ -n "$sz" ] && [ $((0x$sz)) -gt 1100000 ] || {
        echo "far() is ${sz:-?} bytes: no branch in it is past 1 MB"; exit 1; }
fi
sh "$H/link.sh" "$out/far.elf" "$out/far.o" > "$out/ln.log" 2>&1 || {
    echo "-O0: does not link"; head -3 "$out/ln.log"; exit 1; }
got=$(EMBCC_QEMU_TIMEOUT=60 sh "$H/run.sh" "$out/far.elf" 2>&1 | head -1)
[ "$got" = "$want" ] || {
    echo "-O0: disagrees with the host"
    echo "  want: $want"; echo "  got:  $got"; exit 1; }
echo "a conditional branch past 1 MB, forward over an if-arm and backward
around a loop, agrees with the host on a Cortex-M4 at -O0 (far() is
$((0x$sz)) bytes)"
