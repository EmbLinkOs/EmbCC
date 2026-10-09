#!/bin/sh
# A division or remainder of two constants is folded (src/opt/fold.c,
# fold_bin) -- `sizeof a / sizeof a[0]` was a udiv on every target at -O2,
# and a __udivsi3 call on AVR -- except the two that must still happen at
# run time: by zero, and the signed MIN / -1 that overflows, which trap on
# some machines. tests/exec/div-const-fold.c checks the folded VALUES on
# every board; this checks that the folding happens at all.
set -u
echo "TEST-MARKER fold-div"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/fold-div
rm -rf "${out:?}"; mkdir -p "$out"
fail=0

cat > "$out/f.c" <<'CEOF'
static const unsigned char tab[] = { 1, 2, 3, 4, 5, 6, 7 };
unsigned count(void) { return sizeof tab / sizeof tab[0]; }
int sdiv(void) { int a = -7, b = 2; return a / b; }
int smod(void) { int a = -7, b = 2; return a % b; }
unsigned long udiv(void) { unsigned long a = 0xfffffffful, b = 3; return a / b; }
long long ldiv64(void) { long long a = -9000000000000LL, b = 7; return a / b; }
CEOF
cat > "$out/keep.c" <<'CEOF'
int byzero(void) { int a = 7, b = 0; return a / b; }
__INT32_TYPE__ overflow(void) { __INT32_TYPE__ a = -__INT32_MAX__ - 1, b = -1; return a / b; }
long long overflow64(void) { long long a = -9223372036854775807LL - 1, b = -1; return a % b; }
CEOF

for t in x86_64-elf aarch64-elf thumbv7em-none-eabi riscv32-unknown-elf avr; do
    for o in -O1 -O2 -Os; do
        "$EMBCC" inspect ir --target=$t $o "$out/f.c" > "$out/f.ir" 2>&1 ||
            { echo "FAIL: inspect ir $t $o"; fail=1; continue; }
        n=$(grep -cE '= (div|mod)\.' "$out/f.ir") || n=0
        [ "$n" -eq 0 ] || {
            echo "FAIL $t $o: $n divisions of constants left in the IR"
            grep -E '= (div|mod)\.' "$out/f.ir" | head -3 | sed 's/^/     | /'
            fail=1; }
        "$EMBCC" inspect ir --target=$t $o "$out/keep.c" > "$out/k.ir" 2>&1 ||
            { echo "FAIL: inspect ir keep.c $t $o"; fail=1; continue; }
        n=$(grep -cE '= (div|mod)\.' "$out/k.ir") || n=0
        [ "$n" -eq 3 ] || {
            echo "FAIL $t $o: $n of the 3 trapping divisions kept (by zero, MIN / -1)"
            fail=1; }
    done
done
[ $fail = 0 ] && echo "constant divisions fold on 5 targets at -O1, -O2 and -Os; by zero and MIN / -1 are left to run"
exit $fail
