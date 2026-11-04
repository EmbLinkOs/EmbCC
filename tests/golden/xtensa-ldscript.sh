#!/bin/sh
# embld lays an Xtensa image out from a linker script (D-024), as an ESP32's needs: code and
# its literal pools in one region (the instruction RAM), read-only and writable data in another
# (the data RAM: the ESP32's IRAM takes only 32-bit loads, so byte data must not stay with the
# code), .data stored after the code. The program (tests/golden/xtensa-ldscript/) is linked
# under esp32like.ld with a hand-written reset (a script has no -Tstack stub) and the Xtensa
# harness, and must:
#   - lay out as the script says: .text at 0x60010000, .rodata, .data and .bss at 0x60400000;
#   - run on QEMU's sim machine (de212) and exit 42: byte loads from .rodata in its own region,
#     .data copied from its load address, .bss zeroed, the constructor run.
# -Tstack is still refused with -T: the script owns the layout.
set -u
echo "TEST-MARKER xtensa-ldscript"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
T=xtensa-none-elf
here=tests/golden/xtensa-ldscript
out=tests/golden/out/xtensa-ldscript
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

"$EMBCC" --target=$T -O1 -c tests/harness/xtensa/boot.c -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/xtensa/io.c -o "$out/io.o" &&
"$EMBCC" --target=$T -c $here/reset.S -o "$out/reset.o" &&
"$EMBCC" --target=$T -O2 -c $here/prog.c -o "$out/prog.o" || fail "the program does not compile"
"$EMBLD" -T $here/esp32like.ld "$out/reset.o" "$out/boot.o" "$out/io.o" "$out/prog.o" \
    -o "$out/prog.elf" 2> "$out/link.err" || { cat "$out/link.err"; fail "embld refused the script"; }

if command -v python3 >/dev/null 2>&1; then
    python3 $here/sections.py "$out/prog.elf" > "$out/sections.txt"
    grep -q '^\.text 0x60010000 ' "$out/sections.txt" || fail "text: $(cat "$out/sections.txt")"
    grep -q '^\.rodata 0x60400000 ' "$out/sections.txt" || fail "rodata not in DRAM: $(cat "$out/sections.txt")"
    awk '$1 == ".data" || $1 == ".bss" { if ($2 < "0x60400000") bad = 1 } END { exit bad }' \
        "$out/sections.txt" || fail "data: $(cat "$out/sections.txt")"
fi

"$EMBLD" -T $here/esp32like.ld -Tstack 0x60800000 "$out/reset.o" "$out/boot.o" "$out/io.o" \
    "$out/prog.o" -o "$out/stack.elf" > "$out/stack.err" 2>&1 && fail "-Tstack with -T was accepted"
grep -q "Tstack" "$out/stack.err" || fail "-Tstack with -T: $(cat "$out/stack.err")"

QEMU=${EMBCC_QEMU_XTENSA:-qemu-system-xtensa}
if command -v "$QEMU" >/dev/null 2>&1; then
    sh tests/harness/xtensa/run.sh "$out/prog.elf" > "$out/run.txt" 2>&1
    grep -q "==EXIT 42 ==" "$out/run.txt" || { cat "$out/run.txt"; fail "the program did not exit 42"; }
else
    echo "note: $QEMU not found: the layout is checked, the run is not"
fi
echo "PASS: xtensa-ldscript"
