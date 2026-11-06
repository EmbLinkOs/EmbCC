#!/bin/sh
# embas is the assembler for every target: a GNU-syntax file for the
# target --target names (with the target's -m options, a .S through the
# C preprocessor) makes the object `embcc -c` makes of it, named as `as`
# names it; a .asm is NASM syntax, x86's, and refused for another target.
set -u
echo "TEST-MARKER embas-targets"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
EMBAS=${EMBAS:-$PWD/embas}
case $EMBCC in /*) ;; *) EMBCC=$PWD/$EMBCC ;; esac
case $EMBAS in /*) ;; *) EMBAS=$PWD/$EMBAS ;; esac
out=$PWD/tests/golden/out/embas-targets
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBAS" ] || fail "no embas (make embas)"

printf '\t.syntax unified\n\t.thumb\n\t.globl f\n\t.type f, %%function\nf:\tmovs r0, #COUNT\n\tbx lr\n' > "$out/arm.S"
printf '\t.globl f\nf:\tli a0, 5\n\tret\n' > "$out/rv.s"
printf '\t.globl f\nf:\tldi r24, 5\n\tret\n' > "$out/avr.s"
printf '\t.globl f\nf:\tmovl $5, %%eax\n\tret\n' > "$out/x86.s"
n=0
for spec in "thumbv7m-none-eabi:-mcpu=cortex-m4:arm.S" "riscv32-unknown-elf:-march=rv32imc:rv.s" \
            "avr:-mmcu=atmega328p:avr.s" "x86_64-elf::x86.s"; do
    t=${spec%%:*}; rest=${spec#*:}; m=${rest%%:*}; f=${rest#*:}
    (cd "$out" && "$EMBAS" --target="$t" $m -DCOUNT=3 "$f") || fail "embas refused $f for $t"
    o="$out/${f%.*}.o"
    [ -s "$o" ] || fail "embas $f wrote no ${f%.*}.o"
    "$EMBCC" --target="$t" $m -DCOUNT=3 -c "$out/$f" -o "$out/ref.o" || fail "embcc -c $f"
    cmp -s "$o" "$out/ref.o" || fail "embas $f differs from embcc -c for $t"
    n=$((n + 1))
done
echo "$n targets' GNU-syntax files: embas makes embcc -c's objects"

printf 'nop\n' > "$out/k.asm"
"$EMBAS" --target=avr "$out/k.asm" -o "$out/k.o" 2> "$out/k.err" && fail "a .asm for AVR was accepted"
grep -q "x86's" "$out/k.err" || fail "a .asm for AVR was refused, but not by name"
"$EMBAS" "$out/k.asm" -o "$out/k.o" || fail "embas refused k.asm"
echo "a .asm is NASM's, x86's"
