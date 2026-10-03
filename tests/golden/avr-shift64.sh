#!/bin/sh
# 64-bit shifts by a constant on AVR, every count, at -O0.
#
# With the value in its stack slot -- every value, at -O0 -- a constant
# shift ran the one-bit chain across all eight bytes once per bit: forty
# passes and two kilobytes for `x >> 40`, enough that a debug build of a
# program with a few of them no longer fit the part's flash. Whole bytes
# are now moved first. This checks the result for every count 0-63 of
# <<, logical >> and arithmetic >>, against values computed here.
set -u
echo "TEST-MARKER avr-shift64"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: needs python3"; exit 0; }
out=tests/golden/out/avr-shift64
rm -rf "${out:?}"; mkdir -p "$out"
python3 - "$out" <<'PY'
import sys
out = sys.argv[1]
M = (1 << 64) - 1
x = 0xF1E2D3C4B5A69788
xs = x - (1 << 64)
for part in range(8):
    ks = range(part * 8, part * 8 + 8)
    src = ["typedef unsigned long long u64; typedef long long s64;",
           "typedef __UINT32_TYPE__ u32;",
           "void puts_(const char *s);",
           "static volatile u64 vx = 0xF1E2D3C4B5A69788ULL;",
           "static u32 acc;",
           "static void mix(u64 v) { acc = (acc << 5) - acc + (u32)v + (u32)(v >> 32); }"]
    for k in ks:
        src.append(f"__attribute__((noinline)) static u64 l{k}(u64 a) {{ return a << {k}; }}")
        src.append(f"__attribute__((noinline)) static u64 r{k}(u64 a) {{ return a >> {k}; }}")
        src.append(f"__attribute__((noinline)) static s64 a{k}(s64 a) {{ return a >> {k}; }}")
    src += ["int main(void)", "{", "    u64 x = vx;"]
    acc = 0
    for k in ks:
        src.append(f"    mix(l{k}(x)); mix(r{k}(x)); mix((u64)a{k}((s64)x));")
        for v in (x << k, x >> k, xs >> k):
            v &= M
            acc = ((acc << 5) - acc + (v & 0xffffffff) + (v >> 32)) & 0xffffffff
    src.append(f'    puts_(acc == {acc}u ? "shift ok\\n==END==\\n" : "shift BAD\\n==END==\\n");')
    src += ["    for (;;) ;", "}"]
    open(f"{out}/s{part}.c", "w").write("\n".join(src) + "\n")
PY
EMBCC_AVR_HARNESS=$PWD/$out; export EMBCC_AVR_HARNESS
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/io.o" || {
    echo "the harness does not compile"; exit 1; }
for part in 0 1 2 3 4 5 6 7; do
    "$EMBCC" --target=avr -O0 -c "$out/s$part.c" -o "$out/s.o" &&
    sh tests/harness/avr/link.sh "$out/s.elf" "$out/s.o" > "$out/ln.log" 2>&1 || {
        echo "part $part: does not build"; head -3 "$out/ln.log"; exit 1; }
    got=$(EMBCC_QEMU_UNTIL=END sh tests/harness/avr/run.sh "$out/s.elf" \
              2>/dev/null | grep shift)
    [ "$got" = "shift ok" ] || {
        echo "part $part (counts $((part * 8))-$((part * 8 + 7))): $got"; exit 1; }
done
echo "avr-shift64: <<, >> and signed >> of a 64-bit value by every constant count agree at -O0"
