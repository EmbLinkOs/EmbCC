#!/bin/sh
# embflash --stk500v2: an image put into an Arduino Mega 2560 through its
# STK500v2 bootloader.
#
# Against tests/golden/embflash/fakestk500v2.py, a model of the bootloader
# on a pseudo-terminal, written from Atmel's AVR068, that reports any rule
# a programmer breaks (a checksum or sequence number, a page that is not
# whole or aligned, a write with no address, outside programming mode or
# into the bootloader):
#   - a HEX image with pieces in three pages, one partial, and one past 64K
#     words (the address's bit 31) lands byte for byte, the rest of each
#     page the erased 0xff; --verify reads them back;
#   - an ELF for the ATmega2560 goes to --stk500v2 by its core (e_flags'
#     avr6), at the port found, and --board=mega2560 likewise;
#   - a bootloader that misses the first messages is signed on to all the
#     same; --console shows what the program prints;
#   - an image that reaches the bootloader, a part it does not know, a
#     bootloader that never answers and a byte that reads back wrong are
#     each refused with an error that names them, exit status 2.
set -u
EMBFLASH=${EMBFLASH:-./embflash}
EMBCC=${EMBCC:-./embcc}
[ -x "$EMBFLASH" ] || { echo "FAIL: $EMBFLASH is not built (make embflash)"; exit 1; }
echo "TEST-MARKER embflash-stk500v2"
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
out=tests/golden/out/embflash-stk500v2
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
here=tests/golden/embflash

python3 - "$out" <<'PY'
import sys
out = sys.argv[1]
a = bytes((i * 7 + 3) & 0xFF for i in range(400))      # a page and a half
b = bytes(range(0xF0, 0xFA))
c = bytes(range(0x40, 0x60))                           # past 64K words
def rec(addr, data, kind=0):
    r = bytes([len(data), (addr >> 8) & 0xFF, addr & 0xFF, kind]) + data
    return ":" + r.hex().upper() + "%02X" % ((-sum(r)) & 0xFF) + "\n"
def ela(upper):
    return rec(0, bytes([upper >> 8, upper & 0xFF]), 4)
with open(f"{out}/fw.hex", "w") as f:
    f.write(ela(0))
    for i in range(0, len(a), 16):
        f.write(rec(i, a[i:i + 16]))
    f.write(rec(0x600, b))
    f.write(ela(0x0002))
    f.write(rec(0x0010, c[:16]))
    f.write(rec(0x0020, c[16:]))
    f.write(":00000001FF\n")
with open(f"{out}/boot.hex", "w") as f:                 # into the bootloader's 8 KiB
    f.write(ela(0x0003))
    f.write(rec(0xE000, b"\x01\x02"))
    f.write(":00000001FF\n")
with open(f"{out}/want", "wb") as f:
    flash = bytearray(b"\xff" * 262144)
    flash[0:400] = a
    flash[0x600:0x60A] = b
    flash[0x20010:0x20030] = c
    f.write(flash)
PY

# flash NAME "MODEL OPTIONS" EMBFLASH ARGS...: a fresh model, embflash against it
flash() {
    name=$1; mopts=$2; shift 2
    rm -f "$out/$name.port"
    python3 $here/fakestk500v2.py "$out/$name.port" "$out/$name.dump" $mopts > "$out/$name.srv" 2>&1 &
    sp=$!
    i=0
    while [ ! -s "$out/$name.port" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    "$EMBFLASH" "$@" --stk500v2 "serial:$(cat "$out/$name.port")" --timeout 5 > "$out/$name.log" 2>&1
    frc=$?
    i=0
    while kill -0 $sp 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    [ $i -lt 100 ] || { kill $sp; echo "FAIL: $name: the bootloader model did not end"; exit 1; }
    wait $sp
    src=$?
}

flash hex "" "$out/fw.hex" --verify
[ $frc -eq 0 ] || { cat "$out/hex.log" "$out/hex.srv"; fail "hex: embflash exited $frc"; }
[ $src -eq 0 ] || { cat "$out/hex.srv"; fail "hex: the bootloader model saw a broken rule"; }
cmp -s "$out/hex.dump" "$out/want" || fail "hex: the flash is not the image"
grep -q "wrote 4 pages of 256 bytes to the ATmega2560 through its STK500v2 bootloader" "$out/hex.log" ||
    fail "hex: $(cat "$out/hex.log")"
grep -q "verified" "$out/hex.log" || fail "hex: no verification"
echo "  three pages and one past 64K words, byte for byte, read back"

flash slow "--ignore-msgs 3" "$out/fw.hex" --console 2
[ $frc -eq 0 ] && [ $src -eq 0 ] || fail "a bootloader that misses three messages: embflash $frc"
grep -q "hello from the sketch" "$out/slow.log" || fail "--console: $(cat "$out/slow.log")"

flash boot "" "$out/boot.hex"
[ $frc -eq 2 ] && grep -q "where the bootloader begins" "$out/boot.log" || fail "boot: $(cat "$out/boot.log")"
flash sig "--sig 1e9502" "$out/fw.hex"
[ $frc -eq 2 ] && grep -q "signature 1E 95 02: not a part" "$out/sig.log" || fail "unknown part: $(cat "$out/sig.log")"
flash silent "--silent" "$out/fw.hex"
[ $frc -eq 2 ] && grep -q "no STK500v2 bootloader answered" "$out/silent.log" || fail "silent: $(cat "$out/silent.log")"
flash corrupt "--corrupt-read 0x123" "$out/fw.hex" --verify
[ $frc -eq 2 ] && grep -q "verification failed at 0x123" "$out/corrupt.log" || fail "corrupt: $(cat "$out/corrupt.log")"
echo "  the bootloader's 8 KiB, an unknown part, silence and a changed byte refused by name"

# an ATmega2560 ELF: --stk500v2 by its core, at the port found
if [ -x "$EMBCC" ]; then
    printf 'int main(void) { for (;;) ; }\n' > "$out/m.c"
    "$EMBCC" --target=avr -mmcu=atmega2560 "$out/m.c" -o "$out/m.elf" > "$out/m.log" 2>&1 ||
        fail "the ATmega2560 program does not build: $(head -3 "$out/m.log")"
    dev=$out/dev; mkdir -p "$dev"; : > "$dev/cu.usbmodem1101"
    EMBFLASH_DEV=$dev "$EMBFLASH" "$out/m.elf" --timeout 1 > "$out/auto.log" 2>&1
    grep -q "an avr6 program: --stk500v2" "$out/auto.log" &&
        grep -q "the board's port: $dev/cu.usbmodem1101" "$out/auto.log" ||
        fail "an avr6 ELF: $(cat "$out/auto.log")"
    EMBFLASH_DEV=$dev "$EMBFLASH" "$out/fw.hex" --board=mega2560 --timeout 1 > "$out/board.log" 2>&1
    grep -q "the board's port: $dev/cu.usbmodem1101" "$out/board.log" ||
        fail "--board=mega2560: $(cat "$out/board.log")"
    echo "  an avr6 ELF and --board=mega2560 go to --stk500v2 at the port found"
fi
echo "PASS: embflash-stk500v2"
