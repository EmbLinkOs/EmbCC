#!/bin/sh
# EmbCC's AVR vocabulary, against llvm-mc.
#
# tools/avrcheck generates one assembly line per instruction form from the
# SAME walk that emits the bytes, so a form added to emit.c cannot escape
# the referee, and a form printed but not encoded cannot shift every
# comparison after it and blame the wrong instruction.
#
# AVR needs this more than most machines, because its wrong answers are
# all VALID instructions:
#
#  - operand fields are SPLIT. A five-bit register sits across bits 8 and
#    7..4; a six-bit displacement across three fields; an eight-bit
#    immediate across two. Any mistake encodes a different register or a
#    different constant, never an invalid instruction.
#  - three groups have RESTRICTED operands -- immediates reach only
#    r16-r31, adiw/sbiw only r24/r26/r28/r30, ldd/std only Y and Z -- so a
#    wrong register is silently a different register.
#  - the groups OVERLAP. Nibble 0 of the 0x9000 group is `lds`, a 32-bit
#    instruction; routing `ld rd, Z` there emits half an instruction and
#    turns everything after it into garbage.
#
# Two bugs were caught here that hand-checking had passed:
#
#  1. adiw/sbiw's layout is KKdd KKKK -- K split around dd -- not dd
#     followed by a contiguous K. The two encodings agree for (r24, 3)
#     and (r30, 63), which is exactly what the first hand-check used.
#  2. `ld rd, Y` and `ld rd, Z` are the DISPLACED form at zero, not the
#     0x9000 group. The naive encoding produced `lds`.
#
# Which is why the vocabulary sweeps both halves of every split field: an
# r0-r15 and an r16-r31 operand, odd and even registers, the lowest and
# highest immediate, all three pointers, and the displacement values that
# straddle its field boundaries.
set -u
echo "TEST-MARKER avr-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
out=tests/golden/out/avr-encoding
rm -rf "$out"; mkdir -p "$out"

command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc/llvm-objcopy not found"; exit 0; }
"$MC" -triple=avr -mcpu=atmega328p /dev/null -o /dev/null 2>/dev/null || {
    echo "SKIP: this llvm-mc has no AVR target"; exit 0; }

cc -std=c99 -Wall -Wextra -o "$out/avrcheck" \
   tools/avrcheck/avrcheck.c src/arch/avr/emit.c src/arch/code.c \
   src/driver/util.c src/driver/diag.c src/platform/platform_common.c src/platform/platform_posix.c \
   src/arch/target.c src/sema/type.c src/sema/ldfloat.c || {
    echo "avrcheck did not build"; exit 1; }

"$out/avrcheck" --list > "$out/v.s" || {
    echo "avrcheck could not list the vocabulary"; exit 1; }
"$out/avrcheck" bytes > "$out/v.bin" || {
    echo "avrcheck could not encode its own vocabulary"; exit 1; }

n=$(wc -l < "$out/v.s" | tr -d ' ')
[ "$n" -ge 200 ] || {
    echo "the vocabulary is only $n instructions -- it no longer sweeps
both halves of the split fields"; exit 1; }

"$MC" -triple=avr -mcpu=atmega328p -filetype=obj "$out/v.s" -o "$out/v.o" \
    2> "$out/mc.err" || {
    echo "llvm-mc rejected the vocabulary -- an entry claims an"
    echo "        instruction that does not exist:"
    head -4 "$out/mc.err"; exit 1; }
"$OBJCOPY" -O binary --only-section=.text "$out/v.o" "$out/v.ref" 2>/dev/null

cmp -s "$out/v.bin" "$out/v.ref" || {
    echo "an encoding differs from llvm-mc's:"
    # Name the instruction, not the byte. AVR mixes 16- and 32-bit forms
    # so an offset does not divide into a line number -- the two streams
    # are disassembled and diffed instead.
    "$OBJCOPY" --update-section=.text="$out/v.bin" "$out/v.o" "$out/ours.o" \
        2>/dev/null || cp "$out/v.o" "$out/ours.o"
    # Temp files, not process substitution: <(...) is not POSIX sh and
    # this runs under /bin/sh.
    for pair in "ours.o ours.dis" "v.o ref.dis"; do
        set -- $pair
        llvm-objdump -d --triple=avr --mcpu=atmega328p --no-show-raw-insn \
            "$out/$1" 2>/dev/null | sed 's/^ *[0-9a-f]*:\t//' |
            grep -vE '^$|file format|Disassembly|<' > "$out/$2"
    done
    diff "$out/ours.dis" "$out/ref.dis" | head -12
    exit 1; }

# The PC-relative forms, refereed in the OTHER direction. llvm-mc leaves a
# R_AVR_7_PCREL relocation on a branch even to a label in its own section,
# so its bytes are a placeholder and comparing them grades nothing. Our
# bytes are disassembled instead and the text compared against what each
# form was MEANT to be.
#
# This mode exists because its absence cost a miscompile. enum avr_cond
# numbered its flags in mnemonic order rather than by SREG bit, so `breq`
# tested carry and `brlt` tested overflow. Both encoded cleanly, both were
# self-consistent, and `while (*s)` walked past its NUL and printed 1700
# bytes of RAM. A referee that only ever compared this encoder against
# itself could not have seen it.
"$out/avrcheck" --branches > "$out/b.want" || {
    echo "avrcheck could not list the branch forms"; exit 1; }
"$out/avrcheck" branch-bytes > "$out/b.bin" || {
    echo "avrcheck could not encode the branch forms"; exit 1; }
# llvm-mc's own disassembler, fed hex: llvm-objdump cannot read a raw
# binary and these bytes are not in an object. It prints a leading tab,
# which is stripped; the rest of the line is its own spelling and is
# compared as it stands.
od -An -tx1 "$out/b.bin" | tr -s ' ' '\n' | grep -v '^$' | sed 's/^/0x/' |
    tr '\n' ' ' > "$out/b.hex"
llvm-mc -triple=avr -mcpu=atmega328p -disassemble < "$out/b.hex" \
    2> "$out/b.err" | sed 's/^\t//' | grep -v '^$' > "$out/b.cut" || {
    echo "llvm-mc could not disassemble the branch bytes:"
    head -4 "$out/b.err"; exit 1; }
cmp -s "$out/b.want" "$out/b.cut" || {
    echo "a PC-relative form disassembles as something other than what it
        was meant to be -- a condition that encodes cleanly and means the
        wrong thing is exactly this check's job:"
    diff "$out/b.want" "$out/b.cut" | head -12
    exit 1; }
bn=$(wc -l < "$out/b.want" | tr -d ' ')

# What each instruction WRITES, decoded from our bytes by avr_insn_writes
# -- which the code generator's proof that a value kept in a register
# survived each instruction is built on -- against a rule per MNEMONIC
# applied to the text above. llvm-mc has already tied that text to those
# bytes, so these are two readings of one instruction that share no code:
# a decoder that misreads a split field disagrees here, instead of
# certifying that a value outlived an instruction that overwrote it.
"$out/avrcheck" --writes > "$out/w.ours" || {
    echo "avrcheck could not decode its own vocabulary"; exit 1; }
cat "$out/v.s" "$out/b.want" > "$out/all.s"
awk '
function add(r) { got[r] = 1 }
function reg(s) { sub(/^r/, "", s); return s + 0 }
function ptr(p) { if (p ~ /X/) return 26; if (p ~ /Y/) return 28; return 30 }
{
    split("", got)
    m = $1; ops = $0; sub(/^[a-z]+[ \t]*/, "", ops)
    n = split(ops, o, /, */)
    if (m ~ /^(add|adc|sub|sbc|and|or|eor|mov|andi|ori|subi|sbci|ldi|com|neg|swap|inc|dec|asr|lsr|ror|in|pop|lds|ldd|bld)$/)
        add(reg(o[1]))
    else if (m == "movw" || m == "adiw" || m == "sbiw") {
        add(reg(o[1])); add(reg(o[1]) + 1)
    } else if (m ~ /^(mul|muls|mulsu|fmul|fmuls|fmulsu)$/) {
        add(0); add(1)
    } else if (m == "ld") {
        add(reg(o[1]))
        if (o[2] ~ /[+-]/) { add(ptr(o[2])); add(ptr(o[2]) + 1) }
    } else if (m == "st") {
        if (o[1] ~ /[+-]/) { add(ptr(o[1])); add(ptr(o[1]) + 1) }
    } else if (m == "lpm" || m == "elpm") {
        if (ops == "") add(0)
        else { add(reg(o[1])); if (o[2] ~ /[+]/) { add(30); add(31) } }
    } else if (m ~ /^(call|rcall|icall|eicall)$/) {
        add(0); for (r = 18; r <= 27; r++) add(r); add(30); add(31)
    }
    s = ""
    for (r = 0; r < 32; r++) if (r in got) s = s (s == "" ? "" : " ") "r" r
    print (s == "" ? "-" : s)
}' "$out/all.s" > "$out/w.want"
cmp -s "$out/w.ours" "$out/w.want" || {
    echo "the decoder says an instruction writes other registers than its"
    echo "        mnemonic does (instruction | decoded | the rule):"
    paste -d'|' "$out/all.s" "$out/w.ours" "$out/w.want" |
        awk -F'|' '$2 != $3' | head -8
    exit 1; }
wn=$(wc -l < "$out/w.want" | tr -d ' ')

echo "all $n AVR instructions encode as llvm-mc does, across both halves
of every split field and all three pointer registers
and all $bn PC-relative forms disassemble as the condition they name,
which is the half a self-comparison cannot grade
and all $wn decode to the registers their mnemonic writes"
