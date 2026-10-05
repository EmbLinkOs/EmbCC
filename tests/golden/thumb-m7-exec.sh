#!/bin/sh
# tests/exec on a Cortex-M7 with doubles in hardware (-mfpu=fpv5-d16), and
# lib/libc built the same way -- its printf, strtod and fdlibm computing on
# d registers too.
#
# Not every program there is one for a bare 32-bit part: some assume an
# eight-byte `long` or a sixteen-byte `long double`, some need __int128 or a
# host's inline asm. So the reference is the SAME corpus built with doubles
# in SOFTWARE (thumbv7em-none-eabihf alone: the Cortex-M4F's single-precision
# unit, lib/rt/softfp.c for every double) and run on the same board: each
# program that reaches its `// expect-exit` that way must reach it with the
# double-precision unit as well. A program both builds fail is not this
# test's to judge, and the count that must pass keeps the comparison from
# being made over nothing.
set -u
echo "TEST-MARKER thumb-m7-exec"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
"$QEMU" -machine help 2>/dev/null | grep -q mps2-an500 || {
    echo "SKIP: this QEMU has no mps2-an500 (a Cortex-M7)"; exit 0; }

out=tests/golden/out/thumb-m7-exec
rm -rf "$out"; mkdir -p "$out"
T=thumbv7em-none-eabihf
H=$EMBCC_ROOT/tests/harness/thumb-m7

# tools/build-libc.sh and build-rt.sh pass --target= alone, so the M7's
# flag rides in a wrapper.
cat > "$out/m7cc" <<WEOF
#!/bin/sh
exec "$EMBCC" "\$@" -mcpu=cortex-m7
WEOF
chmod +x "$out/m7cc"

# one configuration: its libraries, its harness, and every program's result
corpus() {         # corpus DIR CC
    d=$out/$1; cc=$2
    mkdir -p "$d"
    EMBCC=$cc sh tools/build-libc.sh $T "$d" > "$d/lib.log" 2>&1 &&
    EMBCC=$cc sh tools/build-rt.sh $T "$d" >> "$d/lib.log" 2>&1 || {
        echo "$1: the libraries do not build:"; tail -3 "$d/lib.log"
        return 1; }
    for f in boot io exec; do
        "$cc" --target=$T -O2 -c "$H/$f.c" -o "$d/$f.o" || return 1
    done
    for c in tests/exec/*.c; do
        n=$(basename "$c" .c)
        grep -q '// target:' "$c" && continue
        want=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" | head -1)
        r=OTHER
        if "$cc" --target=$T -O2 -Ilib/libc/include -c "$c" -o "$d/$n.o" \
               2> /dev/null &&
           EMBCC_M7_HARNESS=$PWD/$d sh "$H/link.sh" "$d/$n.elf" "$d/exec.o" \
               "$d/$n.o" "$d/libc.a" "$d/librt.a" > /dev/null 2>&1; then
            EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-10} \
                sh "$H/run.sh" "$d/$n.elf" > "$d/$n.out" 2>&1
            got=$(sed -n 's/.*==EXIT \(-*[0-9]*\) ==.*/\1/p' "$d/$n.out" |
                  head -1)
            [ -n "$got" ] && [ "$got" = "$want" ] && r=PASS
        fi
        echo "$n $r"
    done > "$d/results"
}

corpus soft "$EMBCC" & sp=$!
corpus dp "$PWD/$out/m7cc" & dpp=$!
wait $sp || exit 1
wait $dpp || exit 1

ns=$(grep -c ' PASS$' "$out/soft/results")
nd=$(grep -c ' PASS$' "$out/dp/results")
sort "$out/soft/results" > "$out/soft.sorted"
sort "$out/dp/results" > "$out/dp.sorted"
lost=$(join "$out/soft.sorted" "$out/dp.sorted" |
       awk '$2 == "PASS" && $3 != "PASS" { print $1 }')
if [ -n "$lost" ]; then
    echo "these pass with doubles in software and not on the double-precision
unit:"; echo "$lost"
    exit 1
fi
[ "$nd" -ge 150 ] || {
    echo "only $nd programs reached their expected exit on the M7 -- too few
for the comparison to mean anything"; exit 1; }
echo "tests/exec at -O2 on a Cortex-M7, doubles in hardware and lib/libc built
for it: $nd programs reach their expected exit, every one of the $ns that do
with doubles in software"
