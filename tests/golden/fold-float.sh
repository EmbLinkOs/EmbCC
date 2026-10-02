#!/bin/sh
# Floating-point arithmetic on constants is done by the compiler: at -O1
# and above none of the + - * / or comparisons below may survive into the
# IR, on any target. Every one of them used to be computed at run time --
# `(struct color){ 251/255.0f, ... }` was a divss per component, and
# EmbLinkOs's ui/theme/theme.c five times gcc's size for it. That the
# folded bits are the machine's bits is fold-float-constants.c's job.
set -u
echo "TEST-MARKER fold-float"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/fold-float
rm -rf "${out:?}"; mkdir -p "$out"
cat > "$out/f.c" <<'CEOF'
float  f_div(void)  { return 251 / 255.0f; }
double d_add(void)  { return 0.1 + 0.2; }
float  f_expr(void) { return 2.0f * 3.5f - 1.0f / 4.0f; }
double d_neg(void)  { return -(1.0 / 3.0); }
int    f_cmp(void)  { return 0.5f < 0.25f + 0.5f; }
CEOF
for T in x86_64-elf aarch64-elf thumbv7em-none-eabi thumbv7em-none-eabihf \
         riscv32-unknown-elf riscv64-unknown-elf avr; do
    for opt in -O1 -O2 -Os; do
        "$EMBCC" inspect ir --target=$T $opt "$out/f.c" > "$out/f.ir" 2>&1 || {
            echo "$T $opt: inspect ir failed:"; head -3 "$out/f.ir"; exit 1; }
        # an op whose type suffix carries `f` is floating-point arithmetic
        if grep -E '= (add|sub|mul|div|neg|cmp)\.[0-9]+s?f' "$out/f.ir" > "$out/left"; then
            echo "$T $opt: floating-point arithmetic on constants was not folded:"
            cat "$out/left"; exit 1
        fi
    done
done
echo "float and double arithmetic on constants is folded on every target"
