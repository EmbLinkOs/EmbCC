#!/bin/sh
# MIPS32 assembly files: `embcc -c FILE.S` against clang's assembler.
#
# tests/golden/mips-gas/forms.S is what a MIPS .S file writes: the
# reorder and noreorder modes (and .set push/pop), labels forward and
# back and numeric locals, every branch, j/jal to a local label and to an
# external symbol, %hi/%lo of symbols with addends and of numbers, la,
# loads and stores at %lo(sym)(reg), constant expressions in offsets,
# .equ, the coprocessor-0 and Release 2 instructions, and data in .data,
# .rodata and .bss naming code and data. EmbCC and clang
# (--target=mipsel-unknown-elf -mcpu=mips32r2 -msoft-float -mno-abicalls)
# each assemble it; each object is linked by EmbLD with the same stub,
# and the two images must be the same bytes. The objects themselves may
# relocate differently -- clang names a global branch target where EmbCC
# resolves it, and resolves an .equ where EmbCC relocates against it --
# but they must make the same program.
#
# Then what a .S file may not do here, each refused by name: PIC and
# small-data operators and directives, %lo with an instruction that does
# not sign-extend it, %hi anywhere but a lui, a branch to a symbol this
# file does not define, MIPS16/microMIPS/R6 modes, a hard-float module,
# floating-point instructions, and an unbalanced .set pop.
set -u
# Run BIG-endian (mips-none-elf) as tests/golden/mips-be-gas.sh, which sets
# MIPS_BE=1.
if [ "${MIPS_BE:-0}" = 1 ]; then
    NAME=mips-be-gas T=mips-none-elf MT=mips-unknown-elf
    QEMU=${EMBCC_QEMU_MIPSEB:-qemu-system-mips}
else
    NAME=mips-gas T=mipsel-none-elf MT=mipsel-unknown-elf
    QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
fi
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
CLANG_=${EMBCC_CLANG:-clang}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out"

"$EMBCC" --target=$T -c tests/golden/mips-gas/forms.S -o "$out/forms.o" || {
    echo "forms.S does not assemble"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" -h -S "$out/forms.o" > "$out/forms.hdr" || exit 1
    grep -q 'Flags:.*0x70001001' "$out/forms.hdr" &&
    grep -q '\.rel\.text *REL ' "$out/forms.hdr" &&
    grep -q 'MIPS_ABIFLAGS' "$out/forms.hdr" || {
        echo "an assembled object is not MIPS32r2/o32/noreorder, REL, with .MIPS.abiflags:"
        grep -E 'Flags|rel|ABIFLAGS' "$out/forms.hdr"; exit 1; }
    "$RE" -A "$out/forms.o" | grep -q 'FP ABI: Soft float' || {
        echo "an assembled object's .MIPS.abiflags does not say soft float"; exit 1; }
fi

if command -v "$CLANG_" >/dev/null 2>&1 &&
   "$CLANG_" --target=$MT -mcpu=mips32r2 -msoft-float \
       -mno-abicalls -c tests/golden/mips-gas/forms.S -o "$out/forms-clang.o" \
       2> "$out/clang.err"; then
    printf 'int ext_data[8] = { 1 };\nvoid ext_fn(void) {}\nvoid _start(void) {}\n' \
        > "$out/stub.c"
    "$EMBCC" --target=$T -O1 -c "$out/stub.c" -o "$out/stub.o" || exit 1
    for k in forms forms-clang; do
        # the stub first: its sections then sit where they sit in both
        "$EMBLD" -e _start -Ttext 0x80100000 "$out/stub.o" "$out/$k.o" \
            -o "$out/$k.elf" || { echo "$k.o does not link"; exit 1; }
        "$OD" -s -j .text -j .rodata -j .data -j .bss "$out/$k.elf" |
            tail -n +4 > "$out/$k.img"
    done
    if ! cmp -s "$out/forms.img" "$out/forms-clang.img"; then
        echo "forms.S makes a different program with EmbCC than with clang (clang, then EmbCC):"
        diff "$out/forms-clang.img" "$out/forms.img" | head -10
        exit 1
    fi
    echo "forms.S links to the same image whether EmbCC or clang assembles it"
else
    echo "SKIP the comparison: no clang with a MIPS target"
fi

refuse() {          # refuse WHAT PATTERN SOURCE
    printf '%b\n' "$3" > "$out/bad.S"
    if "$EMBCC" --target=$T -c "$out/bad.S" -o "$out/bad.o" 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refuse ".abicalls" 'position-independent (abicalls)' '.abicalls\nf: jr $ra'
refuse ".cpload" 'position-independent (abicalls)' 'f: .cpload $25'
refuse ".option pic2" 'position-independent (abicalls)' '.option pic2'
refuse "%call16" 'for PIC, small-data or TLS code' 'f: lw $t9, %call16(g)($gp)'
refuse "%gp_rel" 'for PIC, small-data or TLS code' 'f: lw $t0, %gp_rel(g)($gp)'
refuse "ori with %lo" "%lo(symbol) is an addiu's" 'f: ori $t0, $t0, %lo(g)'
refuse "ori with %lo of a number" 'cannot take %hi or %lo' 'f: ori $t0, $t0, %lo(0x18000)'
refuse "%hi off a lui" "%hi(symbol) is a lui's operand" 'f: lw $t0, %hi(g)($t1)'
refuse "a branch to an undefined symbol" 'cannot relocate' 'f: beq $a0, $a1, g'
refuse ".set mips16" '.set mips16 is not supported' '.set mips16'
refuse ".set mips32r6" '.set mips32r6 is not supported' '.set mips32r6'
refuse ".module hardfloat" '.module hardfloat is not supported' '.module hardfloat'
refuse "a floating-point instruction" '"add.s' 'f: add.s $f0, $f1, $f2'
refuse "an unbalanced .set pop" '.set pop with no .set push' '.set pop'
refuse "la of a numeric local" 'la takes a symbol' 'f: la $t0, 1f\n1: nop'
echo "PIC and small-data forms, %lo/%hi misplaced, a branch to an undefined"
echo "symbol, other ISA modes, hard float and FP instructions are refused by name"
