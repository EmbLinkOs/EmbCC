#!/bin/sh
# --board=: a development board's program, and embflash finding its port.
#
#   - the board list (--reference=boards); an unknown board, two boards,
#     and a board with another --target are refused, by name;
#   - an Uno program (--board=uno) runs on QEMU's ATmega328P: printf to the
#     console with "\n" as "\r\n", .bss zero, the constructors run, the
#     board's clock ticking; --board=nano names itself;
#   - an ESP32 program (--board=esp32-devkit) is laid out as the board's
#     runtime has it: the startup in IRAM, the code in flash (IROM), the
#     read-only data in flash (DROM), .data in DRAM; its image carries
#     embflash's boot stub and both regions;
#   - embflash's port (serial:auto, and an ELF with no programmer named),
#     in a stand-in for /dev: the Uno's ATmega16U2 and an ESP32 board's
#     CP2102 side by side are each found, two of a kind are named and
#     neither chosen, none is said so.
set -u
EMBCC=${EMBCC:-./embcc}
EMBFLASH=${EMBFLASH:-./embflash}
echo "TEST-MARKER boards"
out=tests/golden/out/boards
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBFLASH" ] || fail "$EMBFLASH is not built (make embflash)"

# ---- the list, and what is refused ------------------------------------------
"$EMBCC" --reference=boards > "$out/list.md" || fail "--reference=boards"
for b in uno nano esp32-devkit; do
    grep -q "^| \`$b\` |" "$out/list.md" || fail "--reference=boards has no $b"
done
printf 'int main(void) { return 0; }\n' > "$out/m.c"
"$EMBCC" --board=no-such-board -c "$out/m.c" -o "$out/m.o" 2> "$out/unknown.err" &&
    fail "an unknown board was taken"
grep -q "not a board EmbCC knows" "$out/unknown.err" && grep -q "esp32-devkit" "$out/unknown.err" ||
    fail "unknown board: $(cat "$out/unknown.err")"
"$EMBCC" --board=uno --board=esp32-devkit -c "$out/m.c" -o "$out/m.o" 2> "$out/two.err" &&
    fail "two boards were taken"
grep -q "one board at a time" "$out/two.err" || fail "two boards: $(cat "$out/two.err")"
"$EMBCC" --board=uno --target=x86_64-elf -c "$out/m.c" -o "$out/m.o" 2> "$out/tgt.err" &&
    fail "a board with another target was taken"
grep -q "is a avr board" "$out/tgt.err" || fail "board and target: $(cat "$out/tgt.err")"

# ---- the program, for each board ---------------------------------------------
cat > "$out/hello.c" <<'EOF'
#include <board.h>
#include <stdio.h>
static int zeroed[16];
static int ctor_ran;
__attribute__((constructor)) static void mine(void) { ctor_ran = 1; }
int main(void)
{
    int z = 0;
    for (int i = 0; i < 16; i++) z |= zeroed[i];
    printf("hello from %s: %d %s %x, bss %s, ctor %s\n", BOARD_NAME, 42, "ok", 0xbeef,
           z ? "DIRTY" : "zero", ctor_ran ? "ran" : "MISSING");
    unsigned long t0 = board_millis();
    board_delay_ms(20);
    printf("the clock ticks: %s\n", board_millis() - t0 >= 20 ? "yes" : "no");
    printf("<<END>>\n");
    for (;;) ;
}
EOF
"$EMBCC" --board=uno -Os "$out/hello.c" -o "$out/uno.elf" > "$out/uno.log" 2>&1 ||
    fail "--board=uno does not build: $(head -3 "$out/uno.log") (make rt-embedded?)"
if command -v qemu-system-avr > /dev/null 2>&1; then
    EMBCC_QEMU_UNTIL='<<END>>' EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-30} \
        sh tests/harness/avr/run.sh "$out/uno.elf" > "$out/uno.out" 2>/dev/null
    grep -q "hello from Arduino Uno: 42 ok beef, bss zero, ctor ran" "$out/uno.out" ||
        fail "the Uno program on QEMU: $(head -3 "$out/uno.out")"
    grep -q "the clock ticks: yes" "$out/uno.out" || fail "the Uno's clock: $(cat "$out/uno.out")"
    od -c "$out/uno.out" | grep -q '\\r  \\n' || fail "the Uno's console does not send \\r\\n"
    echo "  --board=uno: printf, .bss, the constructors and the clock, on QEMU's ATmega328P"
else
    echo "  (no qemu-system-avr: the Uno program is built, not run)"
fi
"$EMBCC" --board=nano -E -dM "$out/m.c" | grep -q 'define BOARD_NANO 1' ||
    fail "--board=nano does not define BOARD_NANO"
printf '#include <board.h>\nconst char *n = BOARD_NAME;\n' > "$out/name.c"
"$EMBCC" --board=nano -E "$out/name.c" | grep -q '"Arduino Nano"' || fail "--board=nano's BOARD_NAME"

"$EMBCC" --board=esp32-devkit -O2 "$out/hello.c" -o "$out/esp.elf" > "$out/esp.log" 2>&1 ||
    fail "--board=esp32-devkit does not build: $(head -3 "$out/esp.log") (make rt-embedded?)"
"$EMBFLASH" "$out/esp.elf" --dry-run > "$out/esp.pieces" || fail "the ESP32 ELF's pieces"
python3 - "$out/esp.pieces" <<'PY' || fail "the ESP32 program is not laid out as the board's: $(cat "$out/esp.pieces")"
import re, sys
t = open(sys.argv[1]).read()
pieces = [int(a, 16) for a in re.findall(r"0x([0-9a-f]+)\s+\d+ bytes", t)]
entry = int(re.search(r"entry 0x([0-9a-f]+)", t).group(1), 16)
assert 0x40080000 <= entry < 0x400A0000, hex(entry)                   # IRAM
assert any(0x400D0000 <= a < 0x40400000 for a in pieces), "no code in IROM"
assert any(0x3F400000 <= a < 0x3F800000 for a in pieces), "no data in DROM"
assert any(0x3FFB0000 <= a < 0x3FFE0000 for a in pieces), "no .data in DRAM"
PY
"$EMBFLASH" "$out/esp.elf" --esp-image "$out/esp.img" > "$out/img.log" 2>&1 ||
    fail "the ESP32 image: $(cat "$out/img.log")"
grep -q "with 2 flash-mapped regions" "$out/img.log" || fail "the ESP32 image: $(cat "$out/img.log")"
echo "  --board=esp32-devkit: startup in IRAM, code and constants in flash, the boot stub's image"

# ---- the port ------------------------------------------------------------------
dev=$out/dev
port() { # NAME... : a stand-in /dev holding these names
    rm -rf "$dev"; mkdir -p "$dev"
    for n in "$@"; do : > "$dev/$n"; done
}
port cu.usbmodem11101 cu.usbserial-0001 cu.Bluetooth-Incoming-Port
EMBFLASH_DEV=$dev "$EMBFLASH" "$out/uno.elf" --timeout 1 > "$out/p-uno.log" 2>&1
grep -q "an AVR program: --stk500" "$out/p-uno.log" &&
    grep -q "the board's port: $dev/cu.usbmodem11101" "$out/p-uno.log" ||
    fail "the Uno's port beside an ESP32's: $(cat "$out/p-uno.log")"
EMBFLASH_DEV=$dev "$EMBFLASH" "$out/esp.elf" --timeout 1 > "$out/p-esp.log" 2>&1
grep -q "an Xtensa program: --esp" "$out/p-esp.log" &&
    grep -q "the board's port: $dev/cu.usbserial-0001" "$out/p-esp.log" ||
    fail "the ESP32's port beside an Uno's: $(cat "$out/p-esp.log")"
EMBFLASH_DEV=$dev "$EMBFLASH" "$out/esp.elf" --board=uno --timeout 1 > "$out/p-board.log" 2>&1
grep -q "the board's port: $dev/cu.usbmodem11101" "$out/p-board.log" ||
    fail "--board=uno: $(cat "$out/p-board.log")"
port cu.usbserial-1 cu.usbserial-2
EMBFLASH_DEV=$dev "$EMBFLASH" "$out/esp.elf" --esp serial:auto > "$out/p-two.log" 2>&1 &&
    fail "two ports of a kind: one was chosen"
grep -q "2 ports could be the board's" "$out/p-two.log" || fail "two ports: $(cat "$out/p-two.log")"
port ttyS0
EMBFLASH_DEV=$dev "$EMBFLASH" "$out/uno.elf" --stk500 serial:@57600 > "$out/p-none.log" 2>&1 &&
    fail "no port: something was chosen"
grep -q "no USB serial port found" "$out/p-none.log" || fail "no port: $(cat "$out/p-none.log")"
echo "  the port: the Uno's and an ESP32's side by side each found, two of a kind named, none said"

echo "PASS: boards"
