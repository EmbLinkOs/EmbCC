#!/bin/sh
# An assembler fed input it does not understand must stop -- with an
# error, or by assembling what it can -- and never loop.
#
# The MIPS, LoongArch and Xtensa assemblers share an operand splitter
# that did not move past a comma with no operand before it (`nop ,1`): the
# token came out empty, was dropped, and the scan stood still. AVR's
# harness boot.S, fed to any of the three, ran for 39 CPU-minutes. Each
# assembler here gets such lines and another architecture's .S files, and
# each run must end within 20 seconds.
set -u
echo "TEST-MARKER asm-foreign-input"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/asm-foreign-input
rm -rf "$out"; mkdir -p "$out"
printf 'nop ,1\naddi ,a1, 1\n,\nfoo ,,\n' > "$out/commas.s"

# runs TARGET FILE: 0 when the assembler finished, 1 when it had to be killed
runs() {
    "$EMBCC" --target="$1" -c "$2" -o "$out/x.o" > "$out/log" 2>&1 &
    p=$!
    i=0
    while kill -0 $p 2>/dev/null && [ $i -lt 200 ]; do sleep 0.1; i=$((i + 1)); done
    if kill -0 $p 2>/dev/null; then
        kill -9 $p 2>/dev/null
        wait $p 2>/dev/null
        return 1
    fi
    wait $p 2>/dev/null
    return 0
}

n=0
for t in mipsel-none-elf mips-none-elf loongarch64-unknown-elf xtensa-none-elf; do
    for f in "$out/commas.s" tests/harness/avr/boot.S \
             tests/harness/thumb-m0/boot.S tests/harness/riscv/boot.S; do
        [ -f "$f" ] || continue
        if ! runs $t "$f"; then
            echo "FAIL: the $t assembler did not finish on $f"
            exit 1
        fi
        n=$((n + 1))
    done
done
echo "asm-foreign-input: $n assemblies of foreign or malformed input each ended"
