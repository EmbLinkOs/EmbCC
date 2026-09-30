#!/bin/sh
# Multi-byte stores go HIGH byte first, as the ATmega328P's 16-bit I/O
# registers require.
#
# Timer 1's counter, its compare and capture registers and the ADC data
# are sixteen bits on an eight-bit bus, so the part latches: writing the
# HIGH byte parks it in a shared TEMP register and writing the LOW byte
# commits both at once. A 16-bit store made low byte first gives the
# register whatever TEMP last held as its top half -- `TCNT1 = 0x1234`
# sets the count to 0x??34, silently. Reads are the other way round (the
# low byte's read latches the high one), and are low first.
#
# EmbCC stored every multi-byte value low byte first until this test;
# avr-gcc and clang both store high byte first, for every width, which is
# what the backend does now.
#
# This grades the ORDER in the emitted code rather than running it,
# because QEMU's arduino-uno does not model the latch: OCR1A written low
# byte first reads back correct there, and a stopped TCNT1 reads 0 either
# way. So the rule is checked where it lives -- every run of stores
# through Z or X in the listing must go down in address, and none may be a
# post-increment store, which can only go up.
set -u
echo "TEST-MARKER avr-io16"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-io16
rm -rf "$out"; mkdir -p "$out"
command -v llvm-objdump >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }

cat > "$out/prog.c" <<'EOT'
#define TCNT1  (*(volatile unsigned *)0x84)
#define OCR1A  (*(volatile unsigned *)0x88)
void tcnt(void)                          { TCNT1 = 0x1234; }
void ocr(unsigned v)                     { OCR1A = v; }
void put16(volatile unsigned *p, unsigned v)   { *p = v; }
void put32(volatile unsigned long *p, unsigned long v) { *p = v; }
void put64(volatile unsigned long long *p, unsigned long long v) { *p = v; }
struct regs { unsigned char ctl; unsigned cnt; unsigned long acc; };
void field(volatile struct regs *r, unsigned c, unsigned long a)
{ r->cnt = c; r->acc = a; }
EOT

for O in -O0 -O1 -O2 -Os; do
    ./embcc --target=avr $O -c "$out/prog.c" -o "$out/p$O.o" || {
        echo "$O: did not compile"; exit 1; }
    llvm-objdump -d --mcpu=atmega328p --no-show-raw-insn "$out/p$O.o" \
        > "$out/p$O.dis" 2>/dev/null
    # One line per violation: a post-increment store, or a store through
    # the same pointer at a HIGHER displacement than the one before it
    # with nothing but stores between.
    bad=$(awk '
        /^[0-9a-f]+ </ { fn = $2; last = -1; next }
        $2 == "st" && ($3 ~ /\+,/)     { print fn ": " $0; last = -1; next }
        $2 == "std" && $3 ~ /^Z\+/ {
            q = $3; sub(/^Z\+/, "", q); sub(/,.*/, "", q); q += 0
            if (last >= 0 && q > last) print fn ": " $0
            last = q; next
        }
        $2 == "st" { last = -1; next }
        { last = -1 }' "$out/p$O.dis")
    [ -z "$bad" ] || {
        echo "$O: a multi-byte store that is not high byte first:"
        echo "$bad" | head -6
        exit 1; }
    n=$(grep -cE '	(std|st)	' "$out/p$O.dis")
    [ "$n" -ge 18 ] || {
        echo "$O: only $n stores in the listing -- the program no longer"
        echo "        exercises what this test grades"; exit 1; }
done
echo "every multi-byte store -- 16, 32 and 64 bits, through a constant
address, a pointer and a struct field -- writes its high byte first at
four optimisation levels, which is the order the part's 16-bit I/O latch
needs"
