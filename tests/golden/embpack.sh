#!/bin/sh
# embpack: a linked image as bin, Intel HEX, S-records and UF2, refereed
# by llvm-objcopy (the same bytes, at the same addresses, every checksum
# valid), by QEMU (the raw binary boots on the Cortex-M3 and prints what
# the ELF prints), and by Python's zlib and hashlib for the CRC-32 stamped
# into a symbol, the appended CRC and the manifest's digests. The UF2 file
# is decoded block by block against the binary.
set -u
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
EMBPACK=${EMBPACK:-./embpack}
OC=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
echo "TEST-MARKER embpack"
command -v "$OC" >/dev/null 2>&1 || { echo "SKIP: $OC not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
out=tests/golden/out/embpack
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- the STM32-script firmware (vectors at 0, .data stored after code) ---
d=tests/golden/ldscript
T=thumbv7em-none-eabi
for f in startup prog; do
    "$EMBCC" --target=$T -O2 -c "$d/$f.c" -o "$out/$f.o" || fail "$f.c"
done
"$EMBCC" --target=$T -O2 -c tests/harness/thumb/io.c -o "$out/io.o" || fail io.c
"$EMBLD" -T "$d/stm32.ld" "$out/startup.o" "$out/prog.o" "$out/io.o" \
    -o "$out/fw.elf" 2>/dev/null || fail "the STM32 image does not link"
F=$out/fw.elf

"$EMBPACK" "$F" -o "$out/fw.bin" || fail "bin"
"$OC" -O binary "$F" "$out/ref.bin"
cmp -s "$out/fw.bin" "$out/ref.bin" || fail "the raw binary differs from llvm-objcopy's"
"$EMBPACK" "$F" -o "$out/ff.bin" --fill 0xff || fail "bin --fill"
"$OC" -O binary --gap-fill 0xff "$F" "$out/ref-ff.bin"
cmp -s "$out/ff.bin" "$out/ref-ff.bin" || fail "--fill 0xff differs from objcopy's --gap-fill 0xff"

cat > "$out/dec.py" <<'PY'
import sys
def ihex(path):
    m, up = {}, 0
    for l in open(path):
        l = l.strip()
        if not l: continue
        if not l.startswith(':'): raise SystemExit('not a HEX record: ' + l)
        b = bytes.fromhex(l[1:]); n, a, t = b[0], b[1] << 8 | b[2], b[3]
        if sum(b) & 0xff: raise SystemExit('bad HEX checksum: ' + l)
        if len(b) != n + 5: raise SystemExit('bad HEX length: ' + l)
        if t == 0:
            for k in range(n): m[up + a + k] = b[4 + k]
        elif t == 4: up = (b[4] << 8 | b[5]) << 16
    return m
def srec(path):
    m = {}
    for l in open(path):
        l = l.strip()
        if not l: continue
        b = bytes.fromhex(l[2:])
        if (sum(b) & 0xff) != 0xff: raise SystemExit('bad S-record checksum: ' + l)
        if b[0] != len(b) - 1: raise SystemExit('bad S-record count: ' + l)
        al = {'1': 2, '2': 3, '3': 4}.get(l[1])
        if al:
            a = int.from_bytes(b[1:1 + al], 'big')
            for k, v in enumerate(b[1 + al:-1]): m[a + k] = v
    return m
f = ihex if sys.argv[1] == 'hex' else srec
x, y = f(sys.argv[2]), f(sys.argv[3])
if x != y:
    raise SystemExit('%d bytes vs %d, first difference at %s' % (len(x), len(y),
        min((k for k in set(x) | set(y) if x.get(k) != y.get(k)), default=None)))
print('ok', len(x), 'bytes')
PY
"$EMBPACK" "$F" -o "$out/fw.hex" || fail "hex"
"$OC" -O ihex "$F" "$out/ref.hex"
python3 "$out/dec.py" hex "$out/fw.hex" "$out/ref.hex" > "$out/hex.txt" 2>&1 ||
    { cat "$out/hex.txt"; fail "Intel HEX differs from llvm-objcopy's"; }
"$EMBPACK" "$F" -o "$out/fw.srec" || fail "srec"
"$OC" -O srec "$F" "$out/ref.srec"
python3 "$out/dec.py" srec "$out/fw.srec" "$out/ref.srec" > "$out/srec.txt" 2>&1 ||
    { cat "$out/srec.txt"; fail "S-records differ from llvm-objcopy's"; }
echo "bin, hex and srec: the bytes llvm-objcopy writes ($(cat "$out/hex.txt"))"

# the raw binary runs: QEMU loads a non-ELF image at address 0
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
if command -v "$QARM" >/dev/null 2>&1; then
    EMBCC_QEMU_ARM=$QARM sh tests/harness/thumb/run.sh "$out/fw.bin" > "$out/run.txt" 2>&1
    printf 'hello from flash\n42 41 0 1 2 42 1 1 \n43 done\n' > "$out/want.txt"
    cmp -s "$out/run.txt" "$out/want.txt" ||
        { cat "$out/run.txt"; fail "the packed raw binary does not run as the ELF does"; }
    echo "and the raw binary boots on the Cortex-M3"
fi

# ---- UF2, block by block --------------------------------------------------
"$EMBPACK" "$F" -o "$out/fw.uf2" --uf2-family rp2040 || fail "uf2"
python3 - "$out/fw.uf2" "$out/fw.bin" > "$out/uf2.txt" 2>&1 <<'PY' ||
import struct, sys
u = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
if len(u) % 512: raise SystemExit('not whole 512-byte blocks')
n = len(u) // 512; got = {}
for i in range(n):
    blk = u[i * 512:(i + 1) * 512]
    m0, m1, flags, addr, size, seq, total, fam = struct.unpack('<8I', blk[:32])
    if (m0, m1, struct.unpack('<I', blk[508:])[0]) != (0x0A324655, 0x9E5D5157, 0x0AB16F30):
        raise SystemExit('block %d: bad magic' % i)
    if seq != i or total != n: raise SystemExit('block %d: sequence %d of %d' % (i, seq, total))
    if size != 256 or addr % 256: raise SystemExit('block %d: payload %d at %#x' % (i, size, addr))
    if not flags & 0x2000 or fam != 0xe48bff56: raise SystemExit('block %d: family %#x' % (i, fam))
    for k in range(256): got[addr + k] = blk[32 + k]
for a, v in enumerate(b):
    if got.get(a) != v: raise SystemExit('byte %#x: uf2 %s, binary %#x' % (a, got.get(a), v))
print('ok', n, 'blocks')
PY
{ cat "$out/uf2.txt"; fail "the UF2 file is wrong"; }

# ---- the CRC stamped into a symbol, in either byte order ------------------
cat > "$out/crc.c" <<'EOF'
const unsigned image_crc __attribute__((used)) = 0xdeadbeef;
static const char banner[] = "firmware 1.2.3";
int main(void) { return banner[image_crc & 7]; }
EOF
cat > "$out/crc.py" <<'PY'
import json, sys, zlib, subprocess
bin_, base, sym, order = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
b = open(bin_, 'rb').read(); o = sym - base
want = zlib.crc32(b[:o] + b[o + 4:]) & 0xffffffff
got = int.from_bytes(b[o:o + 4], order)
if got != want: raise SystemExit('stamped %#x, the CRC of the rest is %#x' % (got, want))
print('ok %#x' % got)
PY
crc_check() {   # TRIPLE ORDER
    "$EMBCC" --target=$1 -O2 -c "$out/crc.c" -o "$out/crc-$1.o" &&
    "$EMBLD" -e main -Ttext 0x80000000 "$out/crc-$1.o" -o "$out/crc-$1.elf" 2>/dev/null ||
        fail "$1: the CRC image does not build"
    "$EMBPACK" "$out/crc-$1.elf" -o "$out/crc-$1.bin" --crc32 image_crc \
        --manifest "$out/crc-$1.json" || fail "$1: --crc32 fails"
    base=$(python3 -c "import json; print(json.load(open('$out/crc-$1.json'))['base'])")
    sym=$(llvm-nm "$out/crc-$1.elf" | awk '$3 == "image_crc" { print $1 }')
    python3 "$out/crc.py" "$out/crc-$1.bin" "$base" "$((0x$sym))" "$2" > "$out/crc-$1.txt" 2>&1 ||
        { cat "$out/crc-$1.txt"; fail "$1: --crc32 stamped the wrong value"; }
    echo "--crc32 on $1 ($2-endian): $(cat "$out/crc-$1.txt")"
}
crc_check riscv32-unknown-elf little
# ...and that image, at 0x80000000, in HEX and S-records: above 64 KiB, so
# the extended linear address records matter
R=$out/crc-riscv32-unknown-elf.elf
"$EMBPACK" "$R" -o "$out/rv.hex" && "$OC" -O ihex "$R" "$out/rv-ref.hex" &&
    python3 "$out/dec.py" hex "$out/rv.hex" "$out/rv-ref.hex" > "$out/rvhex.txt" 2>&1 ||
    { cat "$out/rvhex.txt"; fail "Intel HEX above 64 KiB differs from llvm-objcopy's"; }
"$EMBPACK" "$R" -o "$out/rv.srec" && "$OC" -O srec "$R" "$out/rv-ref.srec" &&
    python3 "$out/dec.py" srec "$out/rv.srec" "$out/rv-ref.srec" > "$out/rvsrec.txt" 2>&1 ||
    { cat "$out/rvsrec.txt"; fail "S-records above 64 KiB differ from llvm-objcopy's"; }
if "$EMBCC" --target=mips-none-elf -c "$out/crc.c" -o /dev/null 2>/dev/null; then
    crc_check mips-none-elf big
else
    echo "(no mips-none-elf in this compiler: the big-endian CRC is skipped)"
fi

# ---- the appended CRC, the manifest, padding ------------------------------
"$EMBPACK" "$F" -o "$out/ac.bin" --append-crc32 --manifest "$out/ac.json" ||
    fail "--append-crc32"
python3 - "$out/ac.bin" "$out/ac.json" "$out/fw.bin" > "$out/ac.txt" 2>&1 <<'PY' ||
import hashlib, json, sys, zlib
b = open(sys.argv[1], 'rb').read(); m = json.load(open(sys.argv[2])); raw = open(sys.argv[3], 'rb').read()
if b[:-4] != raw: raise SystemExit('the appended image is not the plain binary first')
if int.from_bytes(b[-4:], 'little') != zlib.crc32(raw) & 0xffffffff: raise SystemExit('appended CRC wrong')
if m['size'] != len(b) or m['crc32'] != zlib.crc32(b) & 0xffffffff: raise SystemExit('manifest size/crc32 wrong')
if m['sha256'] != hashlib.sha256(b).hexdigest(): raise SystemExit('manifest sha256 wrong')
print('ok')
PY
{ cat "$out/ac.txt"; fail "the appended CRC or the manifest is wrong"; }
"$EMBPACK" "$F" -o "$out/pad.bin" --pad-to 4K --fill 0xff || fail "--pad-to"
[ "$(wc -c < "$out/pad.bin" | tr -d ' ')" = 4096 ] || fail "--pad-to 4K is not 4096 bytes"
python3 -c "
import sys
p = open('$out/pad.bin','rb').read(); r = open('$out/ff.bin','rb').read()
sys.exit(0 if p[:len(r)] == r and set(p[len(r):]) == {255} else 1)" ||
    fail "--pad-to does not keep the image and fill the rest"
echo "UF2 decoded, --append-crc32, --manifest sha256/crc32 and --pad-to agree with Python"

# ---- refusals --------------------------------------------------------------
"$EMBPACK" "$out/prog.o" -o "$out/x.bin" 2>"$out/r1" && fail "an object file is packed"
grep -q 'not a linked image' "$out/r1" || { cat "$out/r1"; fail "the object refusal does not say why"; }
"$EMBPACK" "$F" -o "$out/x.bin" --pad-to 16 2>/dev/null && fail "--pad-to below the image size is accepted"
"$EMBPACK" "$F" -o "$out/x.bin" --crc32 no_such_symbol 2>/dev/null && fail "--crc32 of a missing symbol is accepted"
"$EMBPACK" "$F" -o "$out/x.bin" --crc32 _sbss 2>"$out/r2" && fail "--crc32 of a .bss symbol is accepted"
grep -q 'not in a stored section' "$out/r2" || { cat "$out/r2"; fail "the .bss refusal does not say why"; }
"$EMBPACK" "$F" -o "$out/x.hex" --append-crc32 2>/dev/null && fail "--append-crc32 on HEX is accepted"
echo "embpack: refusals by name"
