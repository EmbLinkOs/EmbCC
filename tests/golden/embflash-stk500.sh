#!/bin/sh
# embflash --stk500: an image put into an Arduino Uno through Optiboot.
#
# Against tests/golden/embflash/fakeoptiboot.py, a model of the bootloader on
# a pseudo-terminal that reports any rule a programmer breaks (a page that is
# not whole or aligned, a write with no address loaded, a command Optiboot
# does not know):
#   - a HEX image with pieces in three pages, one of them partial, lands byte
#     for byte; the rest of each page it touches is the erased 0xff and pages
#     it does not touch are left alone; --verify reads them back;
#   - an ELF and a raw binary (--base) land the same way;
#   - a bootloader that misses the first syncs is synced with all the same;
#   - --console prints what the program writes once the bootloader starts it,
#     and ends when the port hangs up;
#   - an image that reaches the bootloader, a part it does not know, a
#     bootloader that never answers and a byte that reads back wrong are each
#     refused with an error that names them, exit status 2.
set -u
EMBFLASH=${EMBFLASH:-./embflash}
[ -x "$EMBFLASH" ] || { echo "FAIL: $EMBFLASH is not built (make embflash)"; exit 1; }
echo "TEST-MARKER embflash-stk500"
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
out=tests/golden/out/embflash-stk500
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
here=tests/golden/embflash

# the images: 200 bytes at 0x0000 (a page and a half), 10 at 0x0300
python3 - "$out" <<'EOF'
import sys
out = sys.argv[1]
a = bytes((i * 7 + 3) & 0xFF for i in range(200))
b = bytes(range(0xF0, 0xFA))
def rec(addr, data, kind=0):
    r = bytes([len(data), addr >> 8, addr & 0xFF, kind]) + data
    return ":" + r.hex().upper() + "%02X" % ((-sum(r)) & 0xFF) + "\n"
with open(f"{out}/fw.hex", "w") as f:
    for i in range(0, len(a), 16):
        f.write(rec(i, a[i:i + 16]))
    f.write(rec(0x300, b))
    f.write(":00000001FF\n")
with open(f"{out}/boot.hex", "w") as f:            # into Optiboot's last 512 bytes
    f.write(rec(0x7E00, b"\x01\x02"))
    f.write(":00000001FF\n")
with open(f"{out}/fw.bin", "wb") as f:
    f.write(a)
with open(f"{out}/want", "wb") as f:
    flash = bytearray(b"\xff" * 32768)
    flash[0:200] = a
    flash[0x300:0x30A] = b
    f.write(flash)
EOF

# flash NAME "FAKE OPTIONS" EMBFLASH ARGS...: a fresh model, embflash against
# it; $frc and $src are the two exit statuses, $out/NAME.log embflash's output
flash() {
    name=$1; fopts=$2; shift 2
    rm -f "$out/$name.port"
    python3 $here/fakeoptiboot.py "$out/$name.port" "$out/$name.dump" $fopts \
        > "$out/$name.srv" 2>&1 &
    sp=$!
    i=0
    while [ ! -s "$out/$name.port" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    "$EMBFLASH" "$@" --stk500 "serial:$(cat "$out/$name.port")" --timeout 5 > "$out/$name.log" 2>&1
    frc=$?
    # the model ends by itself: when programming mode is left (0), or when
    # embflash hangs up first (3); one that does not end is stopped here
    i=0
    while kill -0 $sp 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    [ $i -lt 100 ] || { kill $sp; echo "FAIL: $name: the bootloader model did not end"; exit 1; }
    wait $sp
    src=$?
}

flash hex "" "$out/fw.hex" --verify
[ $frc -eq 0 ] || { cat "$out/hex.log"; fail "hex: embflash exited $frc"; }
[ $src -eq 0 ] || { cat "$out/hex.srv"; fail "hex: the bootloader model saw a broken rule"; }
cmp -s "$out/hex.dump" "$out/want" || fail "hex: the flash is not the image"
grep -q "wrote 3 pages of 128 bytes to the ATmega328P" "$out/hex.log" || fail "hex: $(cat "$out/hex.log")"
grep -q "verified" "$out/hex.log" || fail "hex: no verification"

flash bin "" "$out/fw.bin" --base 0
[ $frc -eq 0 ] && [ $src -eq 0 ] || fail "bin: embflash $frc, model $src"
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); w=open(sys.argv[2],'rb').read(); sys.exit(d[:256] != w[:256] or d[0x300:] != b'\xff' * (32768 - 0x300))" \
    "$out/bin.dump" "$out/want" || fail "bin: the flash is not the image"

flash slow "--ignore-syncs 3" "$out/fw.hex"
[ $frc -eq 0 ] && [ $src -eq 0 ] || fail "a bootloader that misses three syncs: embflash $frc"

flash console "" "$out/fw.hex" --console 5
[ $frc -eq 0 ] && [ $src -eq 0 ] || { cat "$out/console.log"; fail "console: embflash $frc, model $src"; }
grep -q "hello from the sketch" "$out/console.log" || fail "console: $(cat "$out/console.log")"

flash boot "" "$out/boot.hex"
[ $frc -eq 2 ] || fail "an image over the bootloader was not refused"
grep -q "where the bootloader begins" "$out/boot.log" || fail "boot: $(cat "$out/boot.log")"

# an ATmega1280 (the Mega 1280's ATmegaBOOT): 256-byte pages
flash m1280 "--sig 1e9703 --flash 131072 --page 256" "$out/fw.hex" --verify
[ $frc -eq 0 ] && [ $src -eq 0 ] || { cat "$out/m1280.log" "$out/m1280.srv"; fail "the 1280: embflash $frc, model $src"; }
grep -q "wrote 2 pages of 256 bytes to the ATmega1280" "$out/m1280.log" || fail "the 1280: $(cat "$out/m1280.log")"
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); w=open(sys.argv[2],'rb').read(); sys.exit(d[:0x8000] != w)" \
    "$out/m1280.dump" "$out/want" || fail "the 1280: the flash is not the image"

flash sig "--sig 1e9502" "$out/fw.hex"
[ $frc -eq 2 ] && grep -q "signature 1E 95 02: not a part" "$out/sig.log" || fail "unknown part: $(cat "$out/sig.log")"

flash silent "--silent" "$out/fw.hex"
[ $frc -eq 2 ] && grep -q "no STK500 bootloader answered" "$out/silent.log" || fail "silent: $(cat "$out/silent.log")"

flash corrupt "--corrupt-read 0x41" "$out/fw.hex" --verify
[ $frc -eq 2 ] && grep -q "verification failed at 0x41" "$out/corrupt.log" || fail "corrupt: $(cat "$out/corrupt.log")"

"$EMBFLASH" "$out/fw.hex" --stk500 serial:/dev/null --run > "$out/run.log" 2>&1
[ $? -eq 2 ] && grep -q "the bootloader starts the program itself" "$out/run.log" || fail "--run with --stk500 was not refused"

echo "PASS: embflash-stk500"
