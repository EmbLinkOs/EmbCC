#!/bin/sh
# MVE (Helium, ARMv8.1-M's vector extension) in the Thumb assembler,
# against llvm-mc: the forms src/arch/thumb/mve.inc holds -- learned from
# llvm-mc by tools/mve-isa -- loads and stores (writeback, a vector base,
# a vector offset with uxtw, the interleaving vld2x/vst4x), arithmetic
# with a vector or a scalar, reductions, moves to and from lanes, the
# immediate forms, comparisons with zr; and the assembler's own: a
# statement predicated in a VPT block (vaddt), vpst and vpt with their
# masks, vctp, vmsr/vmrs p0, and a tail-predicated loop. On a Cortex-M33
# the vector instructions are refused.
set -u
echo "TEST-MARKER thumb-mve"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
here=$PWD/tests/golden/thumb-mve
out=$PWD/tests/golden/out/thumb-mve
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
MC=$(command -v llvm-mc || true)
[ -n "$MC" ] || { echo "SKIP: no llvm-mc"; exit 0; }
"$EMBCC" --target=thumbv8m.main-none-eabihf -mcpu=cortex-m55 -c "$here/mve.s" \
    -o "$out/mve.o" 2> "$out/mve.err" || fail "EmbCC refused mve.s: $(head -4 "$out/mve.err")"
"$MC" -triple=thumbv8.1m.main-none-eabi -mattr=+mve.fp,+fp64,+lob -filetype=obj \
    "$here/mve.s" -o "$out/mve.ref.o" || fail "llvm-mc refused mve.s"
for o in mve mve.ref; do llvm-objcopy -O binary -j .text "$out/$o.o" "$out/$o.bin"; done
cmp -s "$out/mve.bin" "$out/mve.ref.bin" || {
    cmp -l "$out/mve.bin" "$out/mve.ref.bin" | head -4
    fail "mve.s: the bytes differ from llvm-mc's"; }
echo "$(grep -c '^	[a-z]' "$here/mve.s") MVE statements encode as llvm-mc's"
"$EMBCC" --target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -c "$here/mve.s" -o "$out/x.o" \
    2> "$out/x.err" && fail "MVE was accepted for a Cortex-M33"
echo "on a Cortex-M33, refused"
