#!/bin/sh
# EmbCC's AVR assembler, against llvm-mc.
#
# The assembler shares its encoders with the code generator
# (src/arch/avr/emit.c), so what this test adds on top is the PARSING: that
# `ld r5, -Y` reaches the pre-decrement form, that `brlo` and `brcs` are one
# instruction, that `lo8()` selects the low byte and `pm_lo8()` halves the
# address first. A wrong answer to any of those is a valid instruction on
# this machine, which is why it is checked against another assembler rather
# than by reading.
#
# Two halves, and the split is forced rather than stylistic:
#
#   The ordinary forms are compared as BYTES. llvm-mc assembles the same
#   lines and the two images must be identical.
#
#   The PC-relative forms cannot be. llvm-mc leaves an R_AVR_7_PCREL
#   relocation on a branch even to a label in its own section, so its bytes
#   are a placeholder and comparing them would grade nothing. Those are
#   assembled here and DISASSEMBLED, and the text compared against what each
#   form was meant to be -- which is what catches a condition that encodes
#   cleanly and names the wrong SREG flag. That bug is not hypothetical: it
#   shipped in this backend and made `while (*s)` walk past its NUL.
#
# The vocabulary is generated FROM the instruction table, so a mnemonic
# added to src/arch/avr/asm.c cannot escape this test.
set -u
echo "TEST-MARKER avr-asm"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-asm
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc/llvm-objcopy not found"; exit 0; }

cc -std=c99 -Wall -Wextra -o "$out/aas" \
   tools/avrasmcheck/avrasmcheck.c src/arch/avr/asm.c src/arch/avr/emit.c \
   src/arch/code.c src/driver/util.c src/driver/diag.c \
   src/platform/platform_posix.c src/arch/target.c src/sema/type.c \
   src/sema/ldfloat.c || { echo "avrasmcheck did not build"; exit 1; }

# ---- the ordinary forms, byte for byte -------------------------------
"$out/aas" --list > "$out/v.s" || { echo "could not list the vocabulary"
                                    exit 1; }
n=$(wc -l < "$out/v.s" | tr -d ' ')
[ "$n" -ge 140 ] || {
    echo "the vocabulary is only $n lines -- it no longer covers the
instruction set"; exit 1; }
"$out/aas" bytes < "$out/v.s" > "$out/v.bin" 2> "$out/v.err" || {
    echo "EmbCC's assembler rejected its OWN vocabulary:"
    head -4 "$out/v.err"; exit 1; }
# -mcpu=atmega2560: the vocabulary includes elpm, eijmp and eicall, which a
# 328P does not implement. The assembler encodes them because it has no
# device description to know which part it is assembling for, which is what
# avr-as does without -mmcu.
"$MC" -triple=avr -mcpu=atmega2560 -filetype=obj "$out/v.s" -o "$out/v.o" \
    2> "$out/mc.err" || {
    echo "llvm-mc rejected the vocabulary -- an entry claims an instruction"
    echo "        or a syntax that does not exist:"
    head -4 "$out/mc.err"; exit 1; }
"$OBJCOPY" -O binary --only-section=.text "$out/v.o" "$out/v.ref" 2>/dev/null

cmp -s "$out/v.bin" "$out/v.ref" || {
    echo "an instruction assembles differently from llvm-mc:"
    "$OBJCOPY" --update-section=.text="$out/v.bin" "$out/v.o" "$out/ours.o" \
        2>/dev/null || cp "$out/v.o" "$out/ours.o"
    for pair in "ours.o ours.dis" "v.o ref.dis"; do
        set -- $pair
        llvm-objdump -d --triple=avr --mcpu=atmega2560 --no-show-raw-insn \
            "$out/$1" 2>/dev/null | sed 's/^ *[0-9a-f]*:\t//' |
            grep -vE '^$|file format|Disassembly|<' > "$out/$2"
    done
    diff "$out/ours.dis" "$out/ref.dis" | head -12
    exit 1; }

# ---- the PC-relative forms, by disassembly ---------------------------
"$out/aas" --pcrel > "$out/p.want" || { echo "could not list the branches"
                                        exit 1; }
"$out/aas" pcrel-bytes > "$out/p.bin" 2> "$out/p.err" || {
    echo "EmbCC's assembler could not encode its own branch forms:"
    head -4 "$out/p.err"; exit 1; }
od -An -tx1 "$out/p.bin" | tr -s ' ' '\n' | grep -v '^$' | sed 's/^/0x/' |
    tr '\n' ' ' > "$out/p.hex"
"$MC" -triple=avr -mcpu=atmega328p -disassemble < "$out/p.hex" \
    2> "$out/pd.err" | sed 's/^\t//' | grep -v '^$' > "$out/p.got" || {
    echo "llvm-mc could not disassemble the branch bytes:"
    head -4 "$out/pd.err"; exit 1; }
cmp -s "$out/p.want" "$out/p.got" || {
    echo "a PC-relative form disassembles as something other than what it
        was meant to be:"
    diff "$out/p.want" "$out/p.got" | head -12
    exit 1; }
pn=$(wc -l < "$out/p.want" | tr -d ' ')

# ---- a whole source file, end to end ----------------------------------
# The harness's startup: numeric local labels, a backward branch, lo8()/hi8()
# against undefined symbols, `call` to an external, lpm and the X/Y/Z
# addressing modes. Assembled by EmbCC and by llvm-mc, and the two
# DISASSEMBLIES compared -- not the bytes, because llvm-mc relocates its
# local branches where EmbCC resolves them, so the images legitimately
# differ in exactly those fields.
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" \
    2> "$out/b.err" || {
    echo "EmbCC could not assemble the harness startup:"
    head -6 "$out/b.err"; exit 1; }
"$MC" -triple=avr -mcpu=atmega328p -filetype=obj \
    tests/harness/avr/boot.S -o "$out/bootref.o" 2>/dev/null || {
    echo "llvm-mc could not assemble the harness startup"; exit 1; }
for pair in "boot.o b.dis" "bootref.o bref.dis"; do
    set -- $pair
    llvm-objdump -d --triple=avr --mcpu=atmega328p --no-show-raw-insn \
        "$out/$1" 2>/dev/null | sed 's/^ *[0-9a-f]*:\t//' |
        grep -vE '^$|file format|Disassembly|<' > "$out/$2"
done
# Every line must agree except the branch displacements llvm left for its
# linker, which it prints as `.-2`.
diff "$out/b.dis" "$out/bref.dis" > "$out/b.diff" || true
if grep -vE '^[<>-]|^[0-9]|\.-2$|\.[+-][0-9]+$' "$out/b.diff" | grep -q .; then
    echo "the startup assembles differently from llvm-mc in more than the
        relocated branch fields:"
    head -12 "$out/b.diff"; exit 1
fi
if grep '^>' "$out/b.diff" | grep -vE '\.-2$' | grep -q .; then
    echo "llvm-mc produced an instruction EmbCC did not:"
    grep '^>' "$out/b.diff" | head -6; exit 1
fi

echo "all $n instruction forms assemble exactly as llvm-mc does, and all $pn
PC-relative forms disassemble as the condition they name -- the half a
byte comparison cannot grade, because llvm-mc relocates those fields
instead of encoding them
and the harness startup assembles to the same instructions, so this target
needs no other toolchain to turn a .S file into an object"
