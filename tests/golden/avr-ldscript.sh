#!/bin/sh
# embld -T for AVR: an image laid out by a GNU ld script in avr-libc's
# style (tests/golden/avr-ldscript/m328p.ld: flash from 0, data space at
# 0x800000 in the linker's view, SRAM from 0x800100).
#  1. tests/golden/avr-ldscript/prog.c, linked by the script and by
#     -Ttext/-Tdata, at -O0 and -O2, prints the same known text on QEMU's
#     ATmega328P both ways: read-only tables, initialised data, a pointer
#     to data in data, function pointers in data, and .bss.
#  2. The layout is avr-ld's: .data runs at 0x800100 and is stored in
#     flash right after the code.
#  3. A region that is too small is refused as ld refuses it, and so is a
#     script that leaves .rodata in program space, where EmbCC's data-space
#     loads cannot read it.
set -u
echo "TEST-MARKER avr-ldscript"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
out=tests/golden/out/avr-ldscript
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-ldscript
want='zero one two three 
140 12 300000 13 24 -12 abcdefghijklmno
==END=='

for O in -O0 -O2; do
    o=$out/O${O#-O}; mkdir -p "$o"
    "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$o/boot.o" &&
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$o/io.o" &&
    "$EMBCC" --target=avr $O -c lib/rt/avr.c -o "$o/rt.o" &&
    "$EMBCC" --target=avr $O -c $D/prog.c -o "$o/prog.o" || fail "$O: does not compile"
    "$EMBLD" -T $D/m328p.ld "$o/boot.o" "$o/io.o" "$o/rt.o" "$o/prog.o" \
        -o "$o/script.elf" || fail "$O: the script does not link"
    "$EMBLD" -e __vectors -Ttext 0x0 -Tdata 0x100 --rom-limit 32768 \
        "$o/boot.o" "$o/io.o" "$o/rt.o" "$o/prog.o" -o "$o/flags.elf" ||
        fail "$O: -Ttext/-Tdata does not link"
    # 2. the layout
    llvm-readelf -S "$o/script.elf" > "$o/sections.txt" 2>/dev/null
    grep -qE '\.data +PROGBITS +00800100 ' "$o/sections.txt" ||
        { cat "$o/sections.txt"; fail "$O: .data does not run at 0x800100"; }
    set -- $(llvm-readelf -S "$o/script.elf" | awk '$3==".text" { print $5, $7 }')
    [ $# = 2 ] || fail "$O: no .text in the image"
    textend=$((0x$1 + 0x$2))
    dataload=$(llvm-readelf -l "$o/script.elf" | awk '$1=="LOAD" && $3=="0x00800100" { print $4 }')
    [ -n "$dataload" ] || fail "$O: no segment for .data"
    [ $((dataload)) -ge $textend ] && [ $((dataload)) -le $((textend + 1)) ] ||
        fail "$O: .data is stored at $dataload, not right after the code ($textend)"
    if command -v "$QEMU" >/dev/null 2>&1; then
        for how in script flags; do
            EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$o/$how.elf" \
                > "$o/$how.out" 2>&1
            got=$(tr -d '\r' < "$o/$how.out")
            [ "$got" = "$want" ] || { printf '%s\n' "$got" | head -5
                fail "$O: linked by $how, the board prints something else"; }
        done
    fi
done
command -v "$QEMU" >/dev/null 2>&1 && ran="on the board, " || ran="(no $QEMU: not run) "
echo "  linked by the script and by -Ttext/-Tdata, ${ran}at -O0 and -O2; .data at 0x800100, stored after the code"

# 3. what is refused
o=$out/O2
sed 's/LENGTH = 32K/LENGTH = 512/' $D/m328p.ld > "$out/small.ld"
"$EMBLD" -T "$out/small.ld" "$o/boot.o" "$o/io.o" "$o/rt.o" "$o/prog.o" \
    -o "$out/small.elf" 2> "$out/small.err" && fail "a 512-byte flash took the program"
grep -q 'region text overflowed by' "$out/small.err" ||
    { cat "$out/small.err"; fail "the flash overflow is not reported as ld reports it"; }
# the read-only data in program space, with the code
awk '/\*\(\.text\.\*\)/ { print; print "    *(.rodata)"; print "    *(.rodata*)"; next }
     /\*\(\.rodata\)/ || /\*\(\.rodata\*\)/ { next } { print }' $D/m328p.ld > "$out/flash-ro.ld"
grep -q 'rodata' "$out/flash-ro.ld" || fail "could not write the flash-rodata script"
"$EMBLD" -T "$out/flash-ro.ld" "$o/boot.o" "$o/io.o" "$o/rt.o" "$o/prog.o" \
    -o "$out/flash-ro.elf" 2> "$out/flash-ro.err" && fail ".rodata in program space was linked"
grep -q 'EmbCC reads read-only data from RAM on AVR' "$out/flash-ro.err" ||
    { cat "$out/flash-ro.err"; fail ".rodata in program space is not refused by name"; }
echo "  a too-small region and .rodata left in program space are refused"
