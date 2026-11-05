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
#     beyond the flash are each refused with an error that names them;
#   - the rate: 460800 is asked of the ROM by default (CHANGE_BAUDRATE),
#     @921600 is asked for when given, @115200 keeps the ROM's; the model
#     checks the line's rate on every command, so a programmer that does not
#     follow its own change fails; an adapter that cannot carry the rate is
#     named.
# And an ELF, where embcc and embld are built:
#   - --esp-image writes the ESP32 image the ROM boots, its header, checksum
#     and SHA-256 checked, and byte for byte esptool's elf2image where
#     python's esptool is installed;
#   - the ELF itself goes to flash as that image, at 0x1000;
#   - --ram loads its pieces into RAM and starts it (MEM_BEGIN, MEM_DATA,
#     MEM_END), and --console shows what it prints;
#   - an ELF with code and data in flash-mapped memory: the image carries
#     embflash's boot stub (tools/embflash/esp32boot.c) as its entry, with a
#     table that maps each region from the flash page it was written to,
#     and the regions land there; --ram refuses such a piece; the stub's
#     bytes are the ones its source was built to, and it still builds.
# And the ROM's sectors: two pieces at unaligned addresses in one 4 KiB
# sector both land (the ROM refuses an erase off a sector, and a second
# erase would wipe the first piece).
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
# it at serial:PORT$rate; $frc and $src are the exit statuses, $out/NAME.log
# embflash's output
flash() {
    name=$1; mopts=$2; shift 2
    rm -f "$out/$name.port"
    python3 $here/fakeesprom.py "$out/$name.port" "$out/$name.dump" $mopts > "$out/$name.srv" 2>&1 &
    sp=$!
    i=0
    while [ ! -s "$out/$name.port" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    "$EMBFLASH" "$@" --esp "serial:$(cat "$out/$name.port")${rate:-}" --timeout 5 > "$out/$name.log" 2>&1
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
grep -q "at 460800 baud" "$out/bin.log" || fail "bin: not at 460800 by default: $(cat "$out/bin.log")"
grep -q "now at 460800 baud" "$out/bin.srv" || fail "bin: the ROM was not asked for 460800"

rate=@921600; flash r921 "" "$out/fw.bin" --base 0x1000 --verify
[ $frc -eq 0 ] && [ $src -eq 0 ] || { cat "$out/r921.log" "$out/r921.srv"; fail "@921600: embflash $frc, model $src"; }
grep -q "at 921600 baud" "$out/r921.log" || fail "@921600: $(cat "$out/r921.log")"

rate=@115200; flash r115 "" "$out/fw.bin" --base 0x1000 --verify
[ $frc -eq 0 ] && [ $src -eq 0 ] || { cat "$out/r115.log" "$out/r115.srv"; fail "@115200: embflash $frc, model $src"; }
grep -q "now at" "$out/r115.srv" && fail "@115200: the rate was changed"
grep -q "at 115200 baud" "$out/r115.log" || fail "@115200: $(cat "$out/r115.log")"

rate=@921600; flash deaf "--deaf-above 460800" "$out/fw.bin" --base 0x1000
[ $frc -eq 2 ] && grep -q "did not answer at 921600 baud" "$out/deaf.log" || fail "deaf: $(cat "$out/deaf.log")"
rate=

"$EMBFLASH" "$out/fw.bin" --base 0x1000 --esp serial:/dev/null@1234x > "$out/badrate.log" 2>&1
[ $? -eq 2 ] && grep -q "the baud rate after @ is a number" "$out/badrate.log" || fail "badrate: $(cat "$out/badrate.log")"

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

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
if [ -x "$EMBCC" ] && [ -x "$EMBLD" ]; then
    printf 'static const char m[] = "abc";\nvolatile int sink;\nvoid entry(void) { for (const char *p = m; *p; p++) sink += *p; for (;;) ; }\n' > "$out/app.c"
    "$EMBCC" --target=xtensa-esp32-elf -O1 -c "$out/app.c" -o "$out/app.o" &&
        "$EMBLD" -e entry -Ttext 0x40080000 -Tdata 0x3FFB0000 "$out/app.o" -o "$out/app.elf" ||
        fail "the ESP32 program does not build"
    "$EMBFLASH" "$out/app.elf" --esp-image "$out/app.img" > "$out/img.log" 2>&1 ||
        fail "--esp-image: $(cat "$out/img.log")"
    python3 - "$out/app.img" <<'PY' || fail "--esp-image: not an ESP32 image"
import hashlib, struct, sys
d = open(sys.argv[1], "rb").read()
magic, nseg, mode, sf, entry = struct.unpack_from("<BBBBI", d)
assert magic == 0xE9 and mode == 2 and sf == 0x20, d[:8].hex()
assert d[8] == 0xEE and d[23] == 1, d[8:24].hex()       # no WP pin; SHA-256 appended
at, csum = 24, 0xEF
for _ in range(nseg):
    addr, n = struct.unpack_from("<II", d, at)
    assert n % 4 == 0 and (0x3FF80000 <= addr < 0x40000000 or 0x40070000 <= addr < 0x400C0000)
    for c in d[at + 8:at + 8 + n]:
        csum ^= c
    at += 8 + n
at += 15 - at % 16
assert d[at] == csum, (d[at], csum)
assert d[at + 1:] == hashlib.sha256(d[:at + 1]).digest()
PY
    if python3 -c "import esptool" 2>/dev/null; then
        python3 -m esptool --chip esp32 elf2image --flash-mode dio --flash-size 4MB \
            --flash-freq 40m -o "$out/app.esptool.img" "$out/app.elf" > /dev/null 2>&1 &&
            cmp -s "$out/app.img" "$out/app.esptool.img" ||
            fail "--esp-image: not esptool elf2image's bytes"
        echo "  --esp-image: esptool elf2image's bytes"
    fi

    flash elf "" "$out/app.elf" --verify
    [ $frc -eq 0 ] && [ $src -eq 0 ] || { cat "$out/elf.log" "$out/elf.srv"; fail "elf: embflash $frc, model $src"; }
    region "$out/elf.dump" 0x1000 "$out/app.img" || fail "elf: the flash at 0x1000 is not the image"

    flash ram "" "$out/app.elf" --ram --console 1
    [ $frc -eq 0 ] && [ $src -eq 0 ] || { cat "$out/ram.log" "$out/ram.srv"; fail "ram: embflash $frc, model $src"; }
    "$EMBFLASH" "$out/app.elf" --dry-run > "$out/pieces.txt"
    python3 - "$out/ram.dump" "$out/pieces.txt" <<'PY' || fail "ram: what the ROM loaded is not the ELF's pieces"
import re, sys
got = [l.split()[:2] for l in open(sys.argv[1]) if not l.startswith("entry")]
want = re.findall(r"0x([0-9a-f]+)\s+(\d+) bytes", open(sys.argv[2]).read())
assert [(int(a, 16), (int(n) + 3) & ~3) for a, n in want] == [(int(a, 16), int(n)) for a, n in got], (got, want)
PY
    entry=$(sed -n 's/^entry \(0x[0-9a-f]*\) stay 0$/\1/p' "$out/ram.dump")
    [ -n "$entry" ] || fail "ram: MEM_END did not start the program: $(cat "$out/ram.dump")"
    grep -q "hello from RAM at $entry" "$out/ram.log" || fail "ram: --console showed: $(cat "$out/ram.log")"

    # code and read-only data in flash-mapped memory (from 0x400D0020), the
    # rest in DRAM
    "$EMBLD" -e entry -Ttext 0x400D0020 -Tdata 0x3FFB0000 "$out/app.o" -o "$out/irom.elf" ||
        fail "the flash-mapped program does not link"
    "$EMBFLASH" "$out/irom.elf" --dry-run > "$out/irom.pieces"
    "$EMBFLASH" "$out/irom.elf" --esp-image "$out/irom.img" > "$out/irom.log" 2>&1 ||
        fail "flash-mapped: $(cat "$out/irom.log")"
    grep -q "flash-mapped region" "$out/irom.log" || fail "flash-mapped: $(cat "$out/irom.log")"
    python3 - "$out/irom.img" "$out/irom.elf" tools/embflash/esp32boot.inc <<'PY' || fail "flash-mapped: the image, its stub or its table is wrong"
import re, struct, sys
d, elf, inc = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read(), open(sys.argv[3]).read()
stub = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2}),", inc))
entry_off = int(re.search(r"ESP32BOOT_ENTRY (0x[0-9a-f]+)", inc).group(1), 16)
# the ELF's sections that are loaded, by address
shoff, = struct.unpack_from("<I", elf, 32)
shentsize, shnum = struct.unpack_from("<HH", elf, 46)
pieces = []
for i in range(shnum):
    _, typ, flags, addr, off, size = struct.unpack_from("<IIIIII", elf, shoff + i * shentsize)[:6]
    if flags & 2 and typ == 1 and size:
        pieces.append((addr, elf[off:off + size]))
mapped = lambda a: 0x3F400000 <= a < 0x3F800000 or 0x400D0000 <= a < 0x40400000
assert any(mapped(a) for a, _ in pieces), pieces
nseg, entry = d[1], struct.unpack_from("<I", d, 4)[0]
assert entry == 0x400C0000 + entry_off, hex(entry)
at, segs = 24, {}
for _ in range(nseg):
    a, n = struct.unpack_from("<II", d, at)
    segs[a] = d[at + 8:at + 8 + n]
    at += 8 + n
s = segs[0x400C0000]
assert s[:len(stub)] == stub, "the stub is not esp32boot.inc's bytes"
t = struct.unpack_from("<2I", s, 0x200)
elf_entry, = struct.unpack_from("<I", elf, 24)
assert t[0] == elf_entry, (hex(t[0]), hex(elf_entry))
maps = [struct.unpack_from("<3I", s, 0x208 + 12 * i) for i in range(t[1])]
for a, b in pieces:
    if not mapped(a):
        assert segs.get(a, b"")[:len(b)] == b, f"the RAM piece at 0x{a:x}"
        continue
    v, p, n = next(m for m in maps if m[0] <= a < m[0] + m[2] * 0x10000)
    assert p % 0x10000 == 0 and v % 0x10000 == 0 and p >= 0x1000 + at
    at_flat = p + (a - v) - 0x1000
    assert d[at_flat:at_flat + len(b)] == b, f"the piece at 0x{a:x} is not at flash 0x{p + a - v:x}"
PY
    flash xip "" "$out/irom.elf" --verify
    [ $frc -eq 0 ] && [ $src -eq 0 ] || { cat "$out/xip.log" "$out/xip.srv"; fail "xip: embflash $frc, model $src"; }
    region "$out/xip.dump" 0x1000 "$out/irom.img" || fail "xip: the flash from 0x1000 is not the image and its regions"
    "$EMBFLASH" "$out/irom.elf" --esp serial:/dev/null --ram > "$out/iram.log" 2>&1 &&
        fail "--ram took a flash-mapped piece"
    grep -q "flash-mapped memory: --ram loads RAM only" "$out/iram.log" || fail "--ram: $(cat "$out/iram.log")"

    # the stub: built from the source the include names, and it still builds
    want=$(sed -n 's/^#define ESP32BOOT_SOURCE "\(.*\)"$/\1/p' tools/embflash/esp32boot.inc)
    have=$(python3 -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" tools/embflash/esp32boot.c)
    [ "$want" = "$have" ] || fail "esp32boot.inc was built from another esp32boot.c: run tools/embflash/esp32boot.sh (and try it on a board)"
    EMBCC=$EMBCC EMBLD=$EMBLD EMBFLASH=$EMBFLASH sh tools/embflash/esp32boot.sh "$out/esp32boot.inc" > "$out/stub.log" 2>&1 ||
        fail "the boot stub does not build: $(cat "$out/stub.log")"
    echo "  an ELF: its ESP32 image, to flash at 0x1000, to RAM and run with its console"
fi

# two pieces off sector boundaries, in one sector
python3 - "$out" <<'PY'
import sys
def rec(addr, data):
    r = bytes([len(data), addr >> 8, addr & 0xFF, 0]) + data
    return ":" + r.hex().upper() + "%02X" % ((-sum(r)) & 0xFF) + "\n"
open(f"{sys.argv[1]}/sector.hex", "w").write(rec(0x2010, bytes(range(16))) + rec(0x2800, bytes(range(100, 116))) + ":00000001FF\n")
PY
flash sector "" "$out/sector.hex" --verify
[ $frc -eq 0 ] && [ $src -eq 0 ] || { cat "$out/sector.log" "$out/sector.srv"; fail "sector: embflash $frc, model $src"; }
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); sys.exit(d[0x2010:0x2020] != bytes(range(16)) or d[0x2800:0x2810] != bytes(range(100,116)) or d[0x2000:0x2010] != b'\xff'*16)" \
    "$out/sector.dump" || fail "sector: the two pieces did not both land"

echo "PASS: embflash-esp"
