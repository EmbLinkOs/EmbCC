#!/bin/sh
# TriCore inline asm: the vocabulary of src/arch/tricore/asm.c on the board,
# and what it refuses by name.
#
# The template is assembled by tricore/asm.c through the same encoder the
# code generator uses -- the one tests/golden/tricore-encoding.sh referees
# against QEMU's translator -- so what this checks is the rest: that each
# statement form maps to the instruction it names, with its operands in
# the fields they name, and that the operand constraints put values where
# the template reads them. tests/golden/tricore-asm.c runs every form a
# program can observe (data, address, memory, constant, read-write and
# bound-register operands; MTCR/MFCR, the loads and stores, SWAP.W and
# CMPSWAP.W) at -O0..-Os, and must exit 42. Then the templates and operands
# EmbCC must not accept are refused, each by name.
set -u
echo "TEST-MARKER tricore-asm"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_TRICORE:-qemu-system-tricore}
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
T=tricore-none-elf
out=tests/golden/out/tricore-asm
rm -rf "$out"; mkdir -p "$out"

refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "an unknown mnemonic" "'frob' is not an instruction" \
    'void f(void){ __asm__ volatile("frob d2"); }'
refc "a form with the wrong operands" "'ld.w' with these operands" \
    'void f(void){ __asm__ volatile("ld.w d2, d3"); }'
refc "an immediate out of range" "'add' with these operands" \
    'void f(void){ __asm__ volatile("add d2, d3, 300"); }'
refc "a template naming the stack pointer" "keeps for itself" \
    'void f(void){ __asm__ volatile("mov.aa sp, a2"); }'
refc "a clobber of a backend scratch" "keeps for itself" \
    'void f(void){ __asm__ volatile("nop" ::: "a12"); }'
refc "a constraint TriCore does not have" 'is not valid for TriCore' \
    'int f(void){ int x; __asm__ volatile("mov %0, 1" : "=x"(x)); return x; }'
refc "a modifier" "modifier '%w' is not supported" \
    'int f(void){ int x; __asm__ volatile("mov %w0, 1" : "=d"(x)); return x; }'
refc "a register variable outside d0-d7/a2-a7" 'is not supported for TriCore asm' \
    'int f(void){ register int v __asm__("d9") = 1; __asm__ volatile("" : "+d"(v)); return v; }'
refc "a symbol in a template" 'a symbol needs a relocation' \
    'void g(void); void f(void){ __asm__ volatile("call g"); }'
refc "a named label in a template" "labels are numeric" \
    'void f(void){ __asm__ volatile("x: j x"); }'
refc "a branch constant out of range" "does not fit its 4 bits" \
    'void f(int x){ __asm__ volatile("jne %0, 10, .+4" :: "d"(x)); }'
echo "unknown mnemonics, wrong operands, reserved registers, foreign"
echo "constraints and modifiers, symbols and named labels in a template are"
echo "refused by name"

command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped the board: no $QEMU"; exit 0; }
inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -undefined dynamic_lookup -o "$out/putc.so" \
   tests/harness/tricore/putc.c 2>/dev/null ||
    { echo "the harness's output plugin does not build"; exit 1; }
EMBCC_TRICORE_PLUGIN=$PWD/$out/putc.so
EMBCC_TRICORE_HARNESS=$PWD/$out
export EMBCC_TRICORE_PLUGIN EMBCC_TRICORE_HARNESS EMBLD
"$EMBCC" --target=$T -O1 -c tests/harness/tricore/boot.c -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/tricore/io.c -o "$out/io.o" ||
    { echo "the harness does not compile"; exit 1; }
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c tests/golden/tricore-asm.c -o "$out/a.o" ||
        { echo "tricore-asm.c does not compile at $opt"; exit 1; }
    sh tests/harness/tricore/link.sh "$out/a.elf" "$out/a.o" ||
        { echo "tricore-asm.c does not link at $opt"; exit 1; }
    sh tests/harness/tricore/run.sh "$out/a.elf" > "$out/a.out"
    grep -q '==EXIT 42 ==' "$out/a.out" || {
        echo "tricore-asm.c at $opt:"; cat "$out/a.out"; exit 1; }
done
echo "every observable asm form computes what it names on the board at -O0..-Os"
