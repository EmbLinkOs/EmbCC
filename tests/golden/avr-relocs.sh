#!/bin/sh
# embld applies every AVR relocation an assembler writes for an
# ATmega: lo8/hi8/hh8/hhi8 and their _NEG and pm_ forms, ldi of a symbol,
# the six-bit ldd/std displacement and adiw/sbiw immediate, in/out and
# sbi/cbi ports, and data words (call and rcall have goldens of their
# own). It knew eleven; the others
# stopped a clang- or avr-gcc-built object from linking (EmbSim's AVR
# work found R_AVR_LO8_LDI_NEG first).
#
# The referee is the assembler itself, with no linker of its own: one
# template is assembled twice -- once naming symbols, which embld links,
# and once with the numbers embld gave those symbols written in, which
# llvm-mc encodes directly. The two must be the same bytes, in .text and
# in .data.
set -u
echo "TEST-MARKER avr-relocs"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/avr-relocs
rm -rf "${out:?}"; mkdir -p "$out"
MC=${EMBCC_LLVM_MC:-llvm-mc}
LOBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
NM=${EMBCC_LLVM_NM:-llvm-nm}
for tool in "$MC" "$LOBJCOPY" "$NM"; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not found"; exit 0; }
done
EMBLD=${EMBLD:-./embld}
fail=0

# @D@ is a data object, @F@ a function; the others are absolute symbols
# defined in a second object, so each operand reaches the linker as a
# relocation rather than being folded by the assembler.
cat > "$out/tmpl.s" <<'EOF'
.text
.global main, fn, dat
main:
    ldi r24, lo8(-(@D@))
    ldi r25, hi8(-(@D@))
    ldi r26, hh8(@D@)
    ldi r27, hh8(-(@D@))
    ldi r16, pm_lo8(@F@)
    ldi r17, pm_hi8(@F@)
    ldi r18, pm_hh8(@F@)
    ldi r19, pm_lo8(-(@F@))
    ldi r20, pm_hi8(-(@F@))
    ldi r21, pm_hh8(-(@F@))
    ldi r22, lo8(@D@)
    ldi r23, hi8(@D@)
    ldi r28, hhi8(@D@)
    ldi r30, @QQ@
    ldd r24, Y+@QD@
    std Z+@QD@, r25
    adiw r24, @QK@
    sbiw r26, @QK@
    in r24, @QIO@
    out @QIO@, r25
    sbi @QP@, 3
    cbi @QP@, 5
    sbis @QP@, 1
    nop
    ret
fn: ret
.data
dat: .byte 1,2,3,4
.byte @QQ@
.long @D@
.short pm(@F@)
.short gs(@F@)
EOF
cat > "$out/abs.s" <<'EOF'
.global qq, qd, qk, qio, qport
.set qq, 0x5a
.set qd, 37
.set qk, 42
.set qio, 0x3d
.set qport, 0x1b
EOF
sed -e 's/@D@/dat/g' -e 's/@F@/fn/g' -e 's/@QQ@/qq/g' -e 's/@QD@/qd/g' \
    -e 's/@QK@/qk/g' -e 's/@QIO@/qio/g' -e 's/@QP@/qport/g' \
    "$out/tmpl.s" > "$out/rel.s"
"$MC" -triple=avr -mcpu=atmega328p -filetype=obj "$out/rel.s" -o "$out/rel.o" &&
"$MC" -triple=avr -mcpu=atmega328p -filetype=obj "$out/abs.s" -o "$out/abs.o" ||
    { echo "FAIL: llvm-mc does not assemble the template"; exit 1; }
nrel=$(readelf -r "$out/rel.o" 2>/dev/null | grep -c 'R_AVR_') || nrel=0
"$EMBLD" -e main -Ttext 0x0 -Tdata 0x100 "$out/rel.o" "$out/abs.o" \
    -o "$out/rel.elf" > "$out/ld.txt" 2>&1 || {
    echo "FAIL: embld does not link the AVR relocations"
    head -3 "$out/ld.txt" | sed 's/^/     | /'; exit 1; }

# The numbers embld chose, written into the template for llvm-mc.
addr() { "$NM" "$out/rel.elf" | awk -v s="$1" '$3 == s { print "0x" $1 }'; }
D=$(addr dat); F=$(addr fn)
[ -n "$D" ] && [ -n "$F" ] || { echo "FAIL: no address for dat or fn"; exit 1; }
sed -e "s/@D@/$D/g" -e "s/@F@/$F/g" -e 's/@QQ@/0x5a/g' -e 's/@QD@/37/g' \
    -e 's/@QK@/42/g' -e 's/@QIO@/0x3d/g' -e 's/@QP@/0x1b/g' \
    -e "s/pm($F)/$(($F / 2))/" -e "s/gs($F)/$(($F / 2))/" \
    "$out/tmpl.s" > "$out/num.s"
# pm() and gs() of a number still leave a relocation in llvm-mc's object;
# written as the word number they are, nothing is left unencoded -- which
# is checked, since a relocation here would compare against a zero.
"$MC" -triple=avr -mcpu=atmega328p -filetype=obj "$out/num.s" -o "$out/num.o" ||
    { echo "FAIL: llvm-mc does not assemble the numeric template"; exit 1; }
if readelf -r "$out/num.o" 2>/dev/null | grep -q 'R_AVR_'; then
    echo "FAIL: the numeric reference still has relocations"; exit 1
fi
for sec in .text .data; do
    "$LOBJCOPY" -O binary --only-section=$sec "$out/rel.elf" "$out/got$sec" &&
    "$LOBJCOPY" -O binary --only-section=$sec "$out/num.o" "$out/want$sec" ||
        { echo "FAIL: objcopy $sec"; fail=1; continue; }
    if ! cmp -s "$out/got$sec" "$out/want$sec"; then
        echo "FAIL: $sec differs from llvm-mc's encoding of the same numbers"
        cmp -l "$out/got$sec" "$out/want$sec" | head -5 | sed 's/^/     | /'
        fail=1
    fi
done
[ "$nrel" -ge 25 ] || { echo "FAIL: only $nrel relocations in the template's object"; fail=1; }
[ $fail = 0 ] && echo "embld applies $nrel AVR relocations of 21 kinds as llvm-mc encodes the same numbers"
exit $fail
