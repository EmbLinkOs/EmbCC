#!/bin/sh
# A constant in an array index, moved into the access (pass_idxoff).
#
# RISC-V has no indexed addressing, so `a[i - 1]` is an add, a shift, an
# add and the load. `(i - 1) << 2` is `(i << 2) - 4` at any i, so the
# address becomes `(a + (i << 2)) - 4` and the -4 a displacement:
# `a[i - 1] + a[i - 2] * 3 + a[i + 1]` is one shift, one add and three
# loads at -4, -8 and +4, which is what clang emits.
#
# It must NOT happen where it does not pay or is not exact: on Thumb,
# whose loads scale a register themselves, for a LONE access -- but two or
# more sharing the new base pay there too (one add and N loads, where each
# was a subtract and a scaled load), and ARMv6-M, whose loads take no
# negative displacement, never; for an `int` index on a 64-bit
# target, where the extension sits between the add and the shift and
# `(long)(i - 1)` is not `(long)i - 1` once `i - 1` wraps; and when `i - 1`
# is wanted for something else as well, which keeps the add alive.
set -eu
echo "TEST-MARKER index-offset"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/index-offset
rm -rf "$out"; mkdir -p "$out"

cat > "$out/f.c" <<'EOF'
int f(int *a, int i) { return a[i - 1] + a[i - 2] * 3 + a[i + 1]; }
long g(long *a, long i) { return a[i - 1] + a[i + 2]; }
int h(int *a, int i, int *o) { int j = i - 1; *o = j; return a[j]; }
int l(int *a, int i) { return a[i - 1]; }
EOF
ir() {                          # ir TARGET -> $out/TARGET.txt
    "$EMBCC" inspect ir --target="$1" -O2 -c "$out/f.c" -o /dev/null \
        > "$out/$1.txt" 2>&1 || {
        echo "FAIL: could not compile for $1:"; cat "$out/$1.txt"; exit 1; }
}
count() {                       # count TARGET FUNC REGEX -> matches in FUNC
    awk -v f="func @$2 " -v re="$3" 'index($0, f) == 1 { on = 1; next }
        /^}/ { on = 0 } on && $0 ~ re { n++ } END { print n + 0 }' \
        "$out/$1.txt"
}
# the displacements f's three loads should end up with, one base for all
bases() {                       # bases TARGET -> distinct bases of the -4/-8/+4 adds
    awk 'index($0, "func @f ") == 1 { on = 1; next } /^}/ { on = 0 }
         on && /= add\.4s? %[0-9]+, #(-4|-8|4)\t/ { sub(",", "", $4); print $4 }' \
        "$out/$1.txt" | sort -u | wc -l | tr -d ' '
}

ir riscv32-unknown-elf
[ "$(count riscv32-unknown-elf f 'shl\.')" = 1 ] &&
    [ "$(count riscv32-unknown-elf f '#(-4|-8|4)	')" = 3 ] &&
    [ "$(bases riscv32-unknown-elf)" = 1 ] || {
    echo "FAIL: on RV32, f should shift i once and read three displacements"
    echo "      off one base:"; cat "$out/riscv32-unknown-elf.txt"; exit 1; }
echo "RV32: a[i - 1], a[i - 2], a[i + 1] are one shifted base and three displacements"

[ "$(count riscv32-unknown-elf h 'shl\.')" = 1 ] &&
    [ "$(count riscv32-unknown-elf h '#-4	')" = 0 ] || {
    echo "FAIL: on RV32, h needs i - 1 anyway, so a[i - 1] should stay as it"
    echo "      was:"; cat "$out/riscv32-unknown-elf.txt"; exit 1; }
echo "but not when i - 1 is wanted for something else too"

ir riscv64-unknown-elf
[ "$(count riscv64-unknown-elf f 'shl\.')" = 3 ] &&
    [ "$(count riscv64-unknown-elf g 'shl\.')" = 1 ] || {
    echo "FAIL: on RV64 an int index (f) must keep its three shifts and a long"
    echo "      one (g) should share one:"; cat "$out/riscv64-unknown-elf.txt"; exit 1; }
echo "RV64: a long index is rewritten, an int one -- extended after the add -- is not"

ir thumbv7em-none-eabi
[ "$(count thumbv7em-none-eabi f 'shl\.')" = 1 ] &&
    [ "$(bases thumbv7em-none-eabi)" = 1 ] || {
    echo "FAIL: on Thumb-2, f's three loads share a base and should be one"
    echo "      shift and three displacements:"
    cat "$out/thumbv7em-none-eabi.txt"; exit 1; }
[ "$(count thumbv7em-none-eabi l 'shl\.')" = 1 ] &&
    [ "$(count thumbv7em-none-eabi l '#-4	')" = 0 ] || {
    echo "FAIL: Thumb scales the index in the load itself, so a lone a[i - 1]"
    echo "      should be left alone:"; cat "$out/thumbv7em-none-eabi.txt"; exit 1; }
echo "Thumb-2: three loads off one base share it; a lone one is left alone"

ir thumbv6m-none-eabi
[ "$(count thumbv6m-none-eabi f 'shl\.')" = 3 ] || {
    echo "FAIL: ARMv6-M's loads take no negative displacement, so f should"
    echo "      be left alone:"; cat "$out/thumbv6m-none-eabi.txt"; exit 1; }
echo "ARMv6-M: left alone"
