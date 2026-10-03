#!/bin/sh
# FLT_EVAL_METHOD is an integer constant on every target.
#
# <float.h> defines it as __FLT_EVAL_METHOD__, which the Cortex-M, RISC-V
# and AVR predefined-macro tables left out (clang computes it in its
# preprocessor, so `clang -dM` does not list it). In #if it read as 0;
# in an expression it was an undeclared name.
set -u
echo "TEST-MARKER float-eval"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/float-eval
rm -rf "${out:?}"; mkdir -p "$out"
cat > "$out/f.c" <<'SRC'
#include <float.h>
_Static_assert(FLT_EVAL_METHOD == 0, "float is evaluated as float");
int method = FLT_EVAL_METHOD;
SRC
for t in x86_64-elf aarch64-elf thumbv7m-none-eabi thumbv8m.main-none-eabi \
         riscv32-unknown-elf riscv64-unknown-elf avr; do
    "$EMBCC" --target=$t -c "$out/f.c" -o "$out/f.o" 2> "$out/err" || {
        echo "FAIL $t:"; cat "$out/err"; exit 1; }
done
echo "float-eval: FLT_EVAL_METHOD is 0, in #if and in an expression, on every target"
