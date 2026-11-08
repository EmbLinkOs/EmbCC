#!/bin/sh
# A char or short parameter arrives extended by its type where the ABI
# says the caller extends it -- the RISC-V psABI, and Xtensa, whose GCC
# code relies on it -- so the callee does not extend it again
# (fold.c, target_args_extended): RV32's `zext.b a3, s2` on a uint8_t
# parameter, which GCC's code does not have. Elsewhere (Cortex-M: AAPCS
# leaves it to the callee) the extension stays. A spilled one is reloaded
# at its own size and sign (Xtensa: its slot holds only its own bytes):
# tests/exec/many-params.c on the board.
set -u
echo "TEST-MARKER args-extended"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/args-extended
rm -rf "$out"; mkdir -p "$out"
cat > "$out/a.c" <<'EOC'
int u8(unsigned char c) { return c + 1; }
int s16(short s) { return s * 3; }
int s8u(signed char c) { return (unsigned char)c; }
EOC
fail=0
for t in riscv32-none-elf xtensa-esp32-elf thumbv7em-none-eabi; do
    "$EMBCC" inspect ir "$out/a.c" --target=$t -Os > "$out/$t.ir" || exit 1
    n=$(sed -n '/func @u8 /,/^}/p;/func @s16 /,/^}/p' "$out/$t.ir" | grep -c '= ext\.')
    m=$(sed -n '/func @s8u /,/^}/p' "$out/$t.ir" | grep -c '= ext\.')
    case $t in
    thumb*) [ "$n" -ge 2 ] || { echo "FAIL $t: the callee's extensions are gone ($n)"; fail=1; } ;;
    *)      [ "$n" = 0 ] || { echo "FAIL $t: a parameter is extended again ($n)"; fail=1; } ;;
    esac
    # a different extension than the parameter's own stays
    [ "$m" -ge 1 ] || { echo "FAIL $t: (unsigned char) of a signed char parameter was dropped"; fail=1; }
done
[ "$fail" = 0 ] || exit 1
echo "args-extended: RV32 and Xtensa do not extend a char/short parameter again; Cortex-M does; another extension stays"
