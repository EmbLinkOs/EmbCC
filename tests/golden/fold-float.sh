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
# Loops of constant arithmetic, which -O2 unrolls into a chain: the
# unrolled `b *= k` reuses b's name, so every step has several
# definitions, and the fold has to follow the block's own constants
# (opt.c, lk_note) rather than ask for b's one definition.
cat > "$out/loop.c" <<'CEOF'
double d_loop(void) { double b = 1.0; for (int i = 0; i < 4; i++) b *= 2.0; return b; }
float  f_loop(void) { float b = 1.0f; for (int i = 0; i < 6; i++) b = b * 1.1f + 0.3f; return b; }
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
# Only where -O2 unrolls them: the embedded targets keep these loops
# (their floating point is a call, or the body is over the unroller's
# budget), and a loop is not a chain of constants.
for T in x86_64-elf aarch64-elf; do
    "$EMBCC" inspect ir --target=$T -O2 "$out/loop.c" > "$out/l.ir" 2>&1 || {
        echo "$T: inspect ir failed:"; head -3 "$out/l.ir"; exit 1; }
    if grep -E '= (add|sub|mul|div|neg)\.[0-9]+s?f' "$out/l.ir" > "$out/left"; then
        echo "$T -O2: an unrolled loop of constant arithmetic was not folded:"
        cat "$out/left"; exit 1
    fi
done
echo "float and double arithmetic on constants is folded on every target, unrolled loops of it at -O2 too"
