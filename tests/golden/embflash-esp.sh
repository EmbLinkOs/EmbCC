#!/bin/sh
# embflash --esp: an image put into an ESP32's flash through its ROM.
#
# Against tests/golden/embflash/fakeesprom.py, a model of the ESP32 ROM's
# serial download mode on a pseudo-terminal that reports any rule a
# programmer breaks (a block with a wrong checksum, out of sequence or
# outside the erased range, a SLIP frame not escaped, a write before the
# flash is attached):
#   - a raw binary holding every byte value (so SLIP's escapes are used) at
#     --base 0x1000 lands byte for byte and the ROM's MD5 of it matches;
#   - a HEX image with pieces at 0x1000 and 0x10000 lands the same way;
#   - a ROM that misses the first syncs is synced with all the same;
#   - an unknown chip, a ROM that never answers, a wrong MD5 and an image
#     beyond the flash are each refused with an error that names them.
set -u
EMBFLASH=${EMBFLASH:-./embflash}
[ -x "$EMBFLASH" ] || { echo "FAIL: $EMBFLASH is not built (make embflash)"; exit 1; }
echo "TEST-MARKER embflash-esp"
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
out=tests/golden/out/embflash-esp
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
here=tests/golden/embflash

python3 - "$out" <<'EOF'
import sys
out = sys.argv[1]
a = bytes((i * 131 + 7) & 0xFF for i in range(3000)) + bytes(range(256))  # every byte value
b = bytes(range(0x10, 0x40))
with open(f"{out}/fw.bin", "wb") as f:
    f.write(a)
def rec(addr, data, kind=0):
    r = bytes([len(data), (addr >> 8) & 0xFF, addr & 0xFF, kind]) + data
    return ":" + r.hex().upper() + "%02X" % ((-sum(r)) & 0xFF) + "\n"
def ela(upper):  # extended linear address
    return rec(0, bytes([upper >> 8, upper & 0xFF]), 4)
with open(f"{out}/fw.hex", "w") as f:
    f.write(ela(0))
    for i in range(0, 256, 16):
        f.write(rec(0x1000 + i, a[i:i + 16]))
    f.write(ela(0x0001))
    f.write(rec(0x0000, b[:16]))
    f.write(rec(0x0010, b[16:32]))
    f.write(rec(0x0020, b[32:48]))
    f.write(":00000001FF\n")
with open(f"{out}/big.bin", "wb") as f:
    f.write(b"\x00" * 4096)
EOF

# flash NAME "MODEL OPTIONS" EMBFLASH ARGS...: a fresh model, embflash against
# it; $frc and $src are the exit statuses, $out/NAME.log embflash's output
flash() {
    name=$1; mopts=$2; shift 2
    rm -f "$out/$name.port"
    python3 $here/fakeesprom.py "$out/$name.port" "$out/$name.dump" $mopts > "$out/$name.srv" 2>&1 &
    sp=$!
    i=0
    while [ ! -s "$out/$name.port" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    "$EMBFLASH" "$@" --esp "serial:$(cat "$out/$name.port")" --timeout 5 > "$out/$name.log" 2>&1
    frc=$?
    i=0
    while kill -0 $sp 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    [ $i -lt 100 ] || { kill $sp; echo "FAIL: $name: the ROM model did not end"; exit 1; }
    wait $sp
    src=$?
}

region() { # DUMP OFFSET FILE: the dump holds FILE's bytes at OFFSET
    python3 -c "import sys; d=open(sys.argv[1],'rb').read(); o=int(sys.argv[2],0); w=open(sys.argv[3],'rb').read(); sys.exit(d[o:o+len(w)] != w)" "$@"
}

flash bin "" "$out/fw.bin" --base 0x1000 --verify
[ $frc -eq 0 ] || { cat "$out/bin.log"; fail "bin: embflash exited $frc"; }
[ $src -eq 0 ] || { cat "$out/bin.srv"; fail "bin: the ROM model saw a broken rule"; }
region "$out/bin.dump" 0x1000 "$out/fw.bin" || fail "bin: the flash is not the image"
grep -q "wrote 4 blocks of 1024 bytes to the ESP32's flash" "$out/bin.log" || fail "bin: $(cat "$out/bin.log")"
grep -q "verified (MD5)" "$out/bin.log" || fail "bin: no MD5 verification"

flash hex "" "$out/fw.hex" --verify
[ $frc -eq 0 ] && [ $src -eq 0 ] || { cat "$out/hex.log" "$out/hex.srv"; fail "hex: embflash $frc, model $src"; }
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); a=open(sys.argv[2],'rb').read(); sys.exit(d[0x1000:0x1100] != a[:256] or d[0x10000:0x10030] != bytes(range(0x10, 0x40)))" \
    "$out/hex.dump" "$out/fw.bin" || fail "hex: the flash is not the image"

flash slow "--ignore-syncs 3" "$out/fw.bin" --base 0x1000
[ $frc -eq 0 ] && [ $src -eq 0 ] || fail "a ROM that misses three syncs: embflash $frc, model $src"

flash chip "--magic 12345678" "$out/fw.bin" --base 0x1000
[ $frc -eq 2 ] && grep -q "chip-detect value 0x12345678: not a chip" "$out/chip.log" || fail "unknown chip: $(cat "$out/chip.log")"

flash silent "--silent" "$out/fw.bin" --base 0x1000
[ $frc -eq 2 ] && grep -q "no Espressif ROM answered" "$out/silent.log" || fail "silent: $(cat "$out/silent.log")"

flash md5 "--corrupt-md5" "$out/fw.bin" --base 0x1000 --verify
[ $frc -eq 2 ] && grep -q "verification failed: the flash at 0x1000" "$out/md5.log" || fail "md5: $(cat "$out/md5.log")"

flash big "" "$out/big.bin" --base 0x3FF800
[ $frc -eq 2 ] && grep -q "beyond the ESP32's 4 MiB of flash" "$out/big.log" || fail "big: $(cat "$out/big.log")"

echo "PASS: embflash-esp"
