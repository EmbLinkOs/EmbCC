#!/bin/sh
# A va_list in a register, where the list is a bare pointer.
#
# On AAPCS32, RISC-V and Apple's arm64 a va_list is a pointer at the next
# argument, and va_arg reads it and writes back the advanced pointer. It
# took the list's ADDRESS to do so, which kept a local list in memory for
# the whole function: a loop summing va_arg(ap, int) stored the list on
# every trip, and __vformat's every va_arg was a load, an add, a store and
# the load it was for. A local list is now read and written as a
# variable, so a loop over a list passed in has no store at all.
# (tests/exec/va-list-local.c checks that the list still agrees with
# va_start, va_copy and a callee given &ap.) AVR keeps the list in
# memory: with three pointer pairs, a list held in one made __vformat
# longer.
set -u
echo "TEST-MARKER va-list-reg"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/va-list-reg
rm -rf "$out"; mkdir -p "$out"

cat > "$out/vs.c" <<'EOT'
#include <stdarg.h>
int vs(int n, va_list ap)
{
    int s = 0;
    while (n-- > 0)
        s += va_arg(ap, int);
    return s;
}
EOT
for t in thumbv7em-none-eabi riscv32-unknown-elf riscv64-unknown-elf \
         aarch64-apple-darwin; do
    "$EMBCC" --target=$t -O2 -c "$out/vs.c" -o "$out/vs-$t.o" 2> "$out/cc.log" || {
        echo "FAIL: $t: could not compile:"; cat "$out/cc.log"; exit 1; }
    "$OD" -d --no-show-raw-insn "$out/vs-$t.o" > "$out/vs-$t.dis"
    st=$(grep -cE '[[:space:]](str|strb|strh|sw|sd|sh|sb|stp|stur)[[:space:]]' \
         "$out/vs-$t.dis" || true)
    [ "$st" = 0 ] || {
        echo "FAIL: $t: va_arg over a list passed in should store nothing:"
        cat "$out/vs-$t.dis"; exit 1; }
done
echo "va_arg loop over a passed-in list: no store on Thumb, RV32, RV64, Apple arm64"
