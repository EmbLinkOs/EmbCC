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
accs() {            # accs WHAT SYMBOL SOURCE: a template naming SYMBOL is
                    # assembled, and the object relocates against it
    printf '%s\n' "$3" > "$out/acc.c"
    "$EMBCC" --target=$T -c "$out/acc.c" -o "$out/acc.o" 2> "$out/acc.err" ||
        { echo "$1 was refused:"; cat "$out/acc.err"; exit 1; }
    ${EMBCC_LLVM_READELF:-llvm-readelf} -rW "$out/acc.o" |
        grep -qE "[[:space:]]_?$2([[:space:]]|$)" ||
        { echo "$1: no relocation against $2"; exit 1; }
}
refc "an unknown mnemonic" "'frob' is not an instruction" \
    'void f(void){ __asm__ volatile("frob d2"); }'
refc "a form with the wrong operands" "'ld.w' takes no operands shaped" \
    'void f(void){ __asm__ volatile("ld.w d2, d3"); }'
refc "an immediate out of range" "300 does not fit operand 3 of 'add" \
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
accs "a symbol in a template" g \
    'void g(void); void f(void){ __asm__ volatile("call g"); }'
# a named label is the template's own: its branch needs no relocation
printf '%s\n' 'void f(void){ __asm__ volatile("x: j x"); }' > "$out/nlab.c"
"$EMBCC" --target=$T -c "$out/nlab.c" -o "$out/nlab.o" ||
    { echo "a named label in a template was refused"; exit 1; }
${EMBCC_LLVM_READELF:-llvm-readelf} -rW "$out/nlab.o" | grep -q "[[:space:]]x$" &&
    { echo "a template's own label went out as a relocation"; exit 1; }
refc "a branch constant out of range" "10 does not fit operand 2 of 'jne" \
    'void f(int x){ __asm__ volatile("jne %0, 10, .+4" :: "d"(x)); }'
echo "unknown mnemonics, wrong operands, reserved registers, foreign"
echo "constraints and modifiers, symbols and named labels in a template are"
echo "refused by name"

command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped the board: no $QEMU"; exit 0; }
why=$(qemu_plugin tests/harness/tricore/putc.c "$out/putc.so") || {
    echo "skipped the board: the TriCore harness prints through a QEMU plugin, and $why"
    exit 0; }
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
