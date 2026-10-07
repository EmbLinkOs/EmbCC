#!/bin/sh
# A float or double operation repeated on the same operands is computed
# once (value numbering in a block, global CSE across blocks), on every
# target, as an integer one is. EmbCC compiles as if FENV_ACCESS were off,
# so the second computation can only give the first one's bits.
#
# tests/exec/fp-cse.c is the input; it runs on every board as part of the
# exec corpus. Here the IR says:
#   - quot: (x*y)/(x*y) is one multiply (fdlibm's `return (x*y)/(x*y)`
#     was two soft-float calls on Cortex-M, with x and y live across the
#     first); diff: the subtract of the product from itself is kept, not
#     folded to 0 (inf - inf is a NaN);
#   - cmps/fcmps: one comparison;
#   - fsum: the signed integer add and the float add of the same two temps
#     both stay: `flt` is part of the key;
#   - across: the product before the branch is reused inside it.
set -u
echo "TEST-MARKER fp-cse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/fp-cse
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
src=tests/exec/fp-cse.c
fn() { awk -v f="$1" '$0 ~ "^func @" f " .*{" {p=1} p {print} p && /^}/ {exit}' "$ir"; }
cnt() { grep -cE "$1" || true; }
n=0
for t in thumbv7m-none-eabi riscv32-unknown-elf x86_64-elf aarch64-elf avr; do
    for O in -O1 -O2 -Os; do
        ir=$out/$t$O.ir
        "$EMBCC" inspect ir "$src" --target=$t $O > "$ir" 2>&1 ||
            fail "$t $O: inspect ir failed"
        k=$(fn quot | cnt 'mul\.[48]s?f ')
        [ "$k" = 1 ] || { fn quot; fail "$t $O quot: $k multiplies, want 1"; }
        k=$(fn diff | cnt 'mul\.[48]s?f ')
        [ "$k" = 1 ] || { fn diff; fail "$t $O diff: $k multiplies, want 1"; }
        fn diff | grep -qE 'sub\.[48]s?f ' ||
            { fn diff; fail "$t $O diff: p - p is not a float subtract"; }
        k=$(fn fsum | cnt 'add\.4s ')
        j=$(fn fsum | cnt 'add\.4sf ')
        [ "$k" = 1 ] && [ "$j" = 1 ] ||
            { fn fsum; fail "$t $O fsum: $k integer and $j float adds, want 1 and 1"; }
        n=$((n + 1))
        [ "$O" = -O1 ] && continue          # no global CSE at -O1
        for f in cmps fcmps; do
            k=$(fn $f | cnt 'cmp\.[48]f ')
            [ "$k" = 1 ] || { fn $f; fail "$t $O $f: $k comparisons, want 1"; }
        done
        k=$(fn across | cnt 'mul\.[48]s?f ')
        [ "$k" = 1 ] || { fn across; fail "$t $O across: $k multiplies, want 1"; }
    done
done
echo "fp-cse: $n target/level pairs compute each repeated float operation once"
