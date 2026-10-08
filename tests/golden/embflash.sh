#!/bin/sh
# embflash: images put into a target through the GDB remote protocol.
#
# Against tests/golden/embflash/fakegdb.py, a server that models a part's
# flash and reports any rule a programmer breaks (an erase that is not
# whole blocks, a write to bytes not erased, X/M into flash):
#   - one firmware as ELF, Intel HEX, S-records and raw binary (all from
#     embpack) lands byte for byte, erasing exactly the blocks it touches
#     and no others (check.py);
#   - every byte value survives the binary escapes, in vFlashWrite and X,
#     in packets small enough to split it many times;
#   - NAKed packets are sent again; no-ack mode is taken when offered;
#   - with no memory map the image is written as memory, X or (when the
#     server has no X, as QEMU's) M;
#   - --run sets sp, pc and xPSR from the vector table, found by name in
#     the server's target description; --verify catches a wrong byte.
# Then against QEMU's gdbstub (lm3s6965evb, started with no image): the
# program embflash puts there runs and prints what it should.
set -u
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
EMBPACK=${EMBPACK:-./embpack}
EMBFLASH=${EMBFLASH:-./embflash}
[ -x "$EMBFLASH" ] || { echo "FAIL: $EMBFLASH is not built (make embflash)"; exit 1; }
[ -x "$EMBPACK" ] || { echo "FAIL: $EMBPACK is not built (make embpack)"; exit 1; }
echo "TEST-MARKER embflash"
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
out=tests/golden/out/embflash
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
here=tests/golden/embflash
sock=$out/g.sock
n=0

# serve NAME "SERVER OPTIONS" EMBFLASH ARGS...: run embflash against a fresh
# fakegdb; the dump is $out/NAME.dump, embflash's output $out/NAME.log,
# and the two exit statuses $frc and $src.
serve() {
    name=$1; sopts=$2; shift 2
    [ -e "$sock" ] && rm -f "$sock"
    python3 $here/fakegdb.py "$sock" "$out/$name.dump" $sopts \
        > "$out/$name.srv" 2>&1 &
    sp=$!
    i=0
    while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    "$EMBFLASH" "$@" --gdb "unix:$sock" --timeout 10 > "$out/$name.log" 2>&1 &
    fp=$!
    # a programmer that loops forever fails here instead of hanging the run
    ( i=0
      while [ $i -lt 600 ] && kill -0 $fp 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
      kill $fp $sp 2>/dev/null ) &
    wd=$!
    wait $fp
    frc=$?
    wait $sp
    src=$?
    wait $wd
    n=$((n + 1))
}

# ---- one firmware, every format ------------------------------------------
T=thumbv7em-none-eabi
d=tests/golden/ldscript
for f in startup prog; do
    "$EMBCC" --target=$T -O2 -c "$d/$f.c" -o "$out/$f.o" || fail "$f.c"
done
"$EMBCC" --target=$T -O2 -c tests/harness/thumb/io.c -o "$out/io.o" || fail io.c
"$EMBLD" -T "$d/stm32.ld" "$out/startup.o" "$out/prog.o" "$out/io.o" \
    -o "$out/fw.elf" 2>/dev/null || fail "the STM32 image does not link"
"$EMBPACK" "$out/fw.elf" -o "$out/fw.hex" || fail "embpack hex"
"$EMBPACK" "$out/fw.elf" -o "$out/fw.srec" || fail "embpack srec"
"$EMBPACK" "$out/fw.elf" -o "$out/fw.bin" || fail "embpack bin"

FLASH="--flash 0,0x40000,0x800 --ram 0x20000000,0x10000"
for img in fw.elf fw.hex fw.srec fw.bin; do
    base=; [ $img = fw.bin ] && base="--base 0"
    order=; [ $img = fw.srec ] && order=--ram-first
    serve "flash-$img" "$FLASH --nak-every 5 $order" "$out/$img" $base --verify
    [ $frc = 0 ] || fail "$img into flash (rc $frc, server $src): $(tail -2 "$out/flash-$img.log") $(tail -3 "$out/flash-$img.srv")"
    [ $src = 0 ] || fail "$img into flash: the server saw $(grep VIOLATION "$out/flash-$img.dump" | head -2)"
    grep -q "verified" "$out/flash-$img.log" || fail "$img: not verified"
    ref=$out/fw.hex; [ $img = fw.bin ] && ref=$out/fw.bin@0
    python3 $here/check.py "$out/flash-$img.dump" "$ref" flash 0x800 \
        > "$out/flash-$img.chk" || fail "$img into flash: $(head -3 "$out/flash-$img.chk")"
done
serve nak "$FLASH --nak-every 3" "$out/fw.elf" -v
[ $frc = 0 ] && [ $src = 0 ] || fail "with every third packet NAKed: $(tail -2 "$out/nak.log")"
grep -q "resending a packet the server NAKed" "$out/nak.log" || fail "no packet was sent again after a NAK"
python3 $here/check.py "$out/nak.dump" "$out/fw.hex" flash 0x800 > "$out/nak.chk" || \
    fail "after NAKs: $(head -3 "$out/nak.chk")"

# ---- every byte value, split across many packets ---------------------------
# two pieces: one across a block boundary, one blocks further on, so the
# untouched block between them must keep its old contents
python3 - "$out/all.hex" <<'PY'
import sys
def rec(a, t, data):
    b = bytes([len(data), a >> 8 & 0xff, a & 0xff, t]) + data
    return ":" + (b + bytes([-sum(b) & 0xff])).hex().upper()
lines = []
for base in (0x7f0, 0x2000):
    data = bytes(range(256)) * 3
    for k in range(0, len(data), 16):
        lines.append(rec(base + k, 0, data[k:k + 16]))
lines.append(":00000001FF")
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY
serve bytes-flash "$FLASH --packet-size 0x100" "$out/all.hex" --verify
[ $frc = 0 ] && [ $src = 0 ] || fail "every byte value into flash: $(tail -2 "$out/bytes-flash.log") $(grep VIOLATION "$out/bytes-flash.dump" | head -2)"
python3 $here/check.py "$out/bytes-flash.dump" "$out/all.hex" flash 0x800 > "$out/bytes-flash.chk" || \
    fail "every byte value into flash: $(head -3 "$out/bytes-flash.chk")"
serve bytes-x "--ram 0,0x4000 --packet-size 0x100 --noack" "$out/all.hex" --verify
[ $frc = 0 ] && [ $src = 0 ] || fail "every byte value with X: $(tail -2 "$out/bytes-x.log")"
python3 $here/check.py "$out/bytes-x.dump" "$out/all.hex" ram 1 > "$out/bytes-x.chk" || \
    fail "every byte value with X: $(head -3 "$out/bytes-x.chk")"
serve bytes-m "--ram 0,0x4000 --packet-size 0x100 --no-x" "$out/all.hex" --verify
[ $frc = 0 ] && [ $src = 0 ] || fail "every byte value with M: $(tail -2 "$out/bytes-m.log")"
python3 $here/check.py "$out/bytes-m.dump" "$out/all.hex" ram 1 > "$out/bytes-m.chk" || \
    fail "every byte value with M: $(head -3 "$out/bytes-m.chk")"

# ---- no memory map: the image goes in as memory ----------------------------
serve nomap "--ram 0,0x40000 --no-map --no-x" "$out/fw.elf" --verify
[ $frc = 0 ] && [ $src = 0 ] || fail "with no memory map: $(tail -2 "$out/nomap.log")"
python3 $here/check.py "$out/nomap.dump" "$out/fw.hex" ram 1 > "$out/nomap.chk" || \
    fail "with no memory map: $(head -3 "$out/nomap.chk")"

# ---- --run: the registers a Cortex-M takes at reset ------------------------
serve run "$FLASH --m-profile" "$out/fw.elf" --run
[ $frc = 0 ] && [ $src = 0 ] || fail "--run: $(tail -2 "$out/run.log")"
python3 - "$out/fw.bin" "$out/run.dump" > "$out/run.chk" <<'PY' || fail "--run: $(cat "$out/run.chk")"
import struct, sys
sp, rst = struct.unpack("<II", open(sys.argv[1], "rb").read(8))
regs = {}
for line in open(sys.argv[2]):
    w = line.split()
    if w[0] == "reg":
        regs[w[1]] = w[2]
want = {"13": struct.pack("<I", sp).hex(), "15": struct.pack("<I", rst & ~1).hex(),
        "25": "00000001", "c": "continued"}
for k, v in want.items():
    if regs.get(k) != v:
        print("register %s is %s, not %s" % (k, regs.get(k), v))
        sys.exit(1)
PY

# ---- --verify finds a byte that did not take -------------------------------
serve corrupt "$FLASH --corrupt 0x40" "$out/fw.elf" --verify
[ $frc != 0 ] || fail "--verify passed an image with a wrong byte at 0x40"
grep -q "0x40" "$out/corrupt.log" || fail "--verify did not name the wrong byte's address: $(tail -2 "$out/corrupt.log")"

# ---- images refused ---------------------------------------------------------
printf ':0400000001020304F2\n' > "$out/noend.hex"
"$EMBFLASH" "$out/noend.hex" --dry-run > "$out/noend.log" 2>&1 && fail "a HEX file with no end record was taken"
grep -q -- "end-of-file" "$out/noend.log" || fail "noend.hex refused for the wrong reason: $(cat "$out/noend.log")"
printf ':0400000001020304F3\n:00000001FF\n' > "$out/badsum.hex"
"$EMBFLASH" "$out/badsum.hex" --dry-run > "$out/badsum.log" 2>&1 && fail "a HEX record with a bad checksum was taken"
grep -q -- "bad checksum" "$out/badsum.log" || fail "badsum.hex refused for the wrong reason: $(cat "$out/badsum.log")"
printf ':0400000001020304F2\n:0400020005060708E0\n:00000001FF\n' > "$out/overlap.hex"
"$EMBFLASH" "$out/overlap.hex" --dry-run > "$out/overlap.log" 2>&1 && fail "overlapping records were taken"
grep -q -- "overlap" "$out/overlap.log" || fail "overlap.hex refused for the wrong reason: $(cat "$out/overlap.log")"
"$EMBFLASH" "$out/fw.bin" --dry-run > "$out/nobase.log" 2>&1 && fail "a raw binary with no --base was taken"
grep -q -- "--base" "$out/nobase.log" || fail "fw.bin with no --base refused for the wrong reason: $(cat "$out/nobase.log")"
"$EMBFLASH" "$out/fw.elf" --dry-run > "$out/dry.log" 2>&1 || fail "--dry-run: $(tail -2 "$out/dry.log")"
grep -q "entry 0x" "$out/dry.log" || fail "--dry-run shows no entry"

# ---- QEMU's gdbstub: a board started with no program -----------------------
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
if command -v "$QARM" >/dev/null 2>&1; then
    printf 'void puts_(const char *s); void putn(long v);\nint main(void){ puts_("flashed and running "); putn(6 * 7); puts_("\\n"); return 42; }\n' > "$out/hello.c"
    for f in boot io; do
        "$EMBCC" --target=thumbv7m-none-eabi -O1 -c tests/harness/thumb/$f.c -o "$out/$f.o" || fail "$f.c"
    done
    "$EMBCC" --target=thumbv7m-none-eabi -O2 -c "$out/hello.c" -o "$out/hello.o" || fail hello.c
    EMBCC_THUMB_HARNESS=$out sh tests/harness/thumb/link.sh "$out/hello.elf" "$out/hello.o" \
        > "$out/link.log" 2>&1 || fail "hello does not link: $(head -3 "$out/link.log")"
    [ -e "$sock" ] && rm -f "$sock"
    sh tests/harness/qrun.sh 20 "$QARM" -M lm3s6965evb -cpu cortex-m3 -nographic \
        -S -chardev socket,path=$sock,server=on,wait=off,id=g -gdb chardev:g \
        > "$out/qemu.out" 2>&1 &
    qp=$!
    i=0
    while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    "$EMBFLASH" "$out/hello.elf" --gdb "unix:$sock" --verify --run > "$out/qflash.log" 2>&1
    frc=$?
    wait $qp
    [ $frc = 0 ] || fail "into QEMU: $(tail -2 "$out/qflash.log")"
    grep -q "verified" "$out/qflash.log" || fail "into QEMU: not verified"
    grep -q "flashed and running 42" "$out/qemu.out" || \
        fail "the program put into QEMU did not run: $(head -3 "$out/qemu.out")"
    n=$((n + 1))
else
    echo "SKIP: the QEMU half ($QARM not found)"
fi
echo "ok embflash ($n sessions)"
