#!/bin/sh
# TriCore atomics narrower than a word.
#
# SWAP.W and CMPSWAP.W are a word, so a one- or two-byte atomic is a
# CMPSWAP.W loop on the aligned word around it that rewrites only its lane,
# old ^ ((new ^ old) & mask) -- atomic against the neighbouring bytes too,
# since a store to any of them between the load and the CMPSWAP.W makes it
# fail and the loop go round. A compare-and-swap compares the lane, not the
# word: CMPSWAP.W is given the word last seen with the expected value in
# the lane, and a failure that only a neighbour caused goes round. This
# runs RISC-V's and MIPS's program for it,
# tests/golden/riscv-atomics/subword.c: every operation on every lane of
# one word, the whole word printed after each, so a clobbered neighbour
# shows. TriCore is little-endian, so the host computes the same output and
# referees it. (There is no TriCore disassembler here to read the loop in
# the object; tricore-encoding.sh checks every form it uses.)
set -u
echo "TEST-MARKER tricore-atomics"
. "$(dirname "$0")/../lib.sh"

T=tricore-none-elf
QEMU=${EMBCC_QEMU_TRICORE:-qemu-system-tricore}
out=tests/golden/out/tricore-atomics
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
SUB=tests/golden/riscv-atomics/subword.c

# ---- the macros claim exactly the widths that exist -------------------
m=$("$EMBCC" --target=$T --dump-predef 2>/dev/null)
for w in 1 2 4; do
    echo "$m" | grep -q "SYNC_COMPARE_AND_SWAP_$w " || {
        echo "a $w-byte compare-and-swap is not claimed, and the backend has one"
        exit 1; }
done
echo "$m" | grep -q 'SYNC_COMPARE_AND_SWAP_8' && {
    echo "claims an eight-byte compare-and-swap; TriCore has none"; exit 1; }
echo "TriCore claims exactly the one-, two- and four-byte compare-and-swaps"

# ---- a sub-word atomic, on the board, against the host ----------------
command -v "$QEMU" >/dev/null 2>&1 || { echo "(SKIP: no $QEMU)"; exit 0; }
HOSTCC=${HOSTCC:-cc}
"$HOSTCC" -std=c99 -w -O2 -o "$out/sub-host" $SUB tests/harness/thumb/hostio.c ||
    { echo "the host does not build subword.c"; exit 1; }
"$out/sub-host" > "$out/want.txt"

inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -undefined dynamic_lookup -o "$out/putc.so" \
   tests/harness/tricore/putc.c 2>/dev/null ||
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -o "$out/putc.so" tests/harness/tricore/putc.c ||
    { echo "the harness's output plugin does not build"; exit 1; }
EMBCC_TRICORE_PLUGIN=$PWD/$out/putc.so
export EMBCC_TRICORE_PLUGIN

S=$out/s; mkdir -p "$S"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c tests/harness/tricore/$f.c -o "$S/$f.o" ||
        { echo "the harness did not compile"; exit 1; }
done
export EMBCC_TRICORE_HARNESS="$S"
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c $SUB -o "$S/sub.o" 2> "$out/sub.err" || {
        echo "$O: subword.c did not compile:"; head -3 "$out/sub.err"; exit 1; }
    sh tests/harness/tricore/link.sh "$S/sub.elf" "$S/sub.o" ||
        { echo "$O: subword.c did not link"; exit 1; }
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
        sh tests/harness/tricore/run.sh "$S/sub.elf" 2>/dev/null |
        tr -d '\r' | sed -n '1,/^DONE/p' > "$out/got.txt"
    cmp -s "$out/want.txt" "$out/got.txt" || {
        echo "$O: the one- and two-byte atomics differ from the host's:"
        diff "$out/want.txt" "$out/got.txt" | head -8; exit 1; }
done
echo "one- and two-byte atomics on every lane of a word, at -O0/-O1/-O2/-Os, equal the host's on the tricore_testboard"
