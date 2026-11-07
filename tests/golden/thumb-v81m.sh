#!/bin/sh
# ARMv8.1-M Mainline's own instructions in the Thumb assembler (Cortex-M55,
# M85), against llvm-mc: the conditional selects (csel, csinc, csinv, csneg
# and cset, csetm, cinc, cinv, cneg; zr), the low-overhead loops (dls,
# dlstp, wls, wlstp, le, letp, lctp), clrm and vscclrm; and MVE's scalar
# side -- vmrs/vmsr of vpr, p0, fpscr_nzcvqc and fpcxt, and the shifts of
# a register pair or one register (lsll, asrl, lsrl, uqshll... sqrshr).
# On ARMv7-M each is refused by name.
set -u
echo "TEST-MARKER thumb-v81m"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
here=$PWD/tests/golden/thumb-v81m
out=$PWD/tests/golden/out/thumb-v81m
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
MC=$(command -v llvm-mc || true)
[ -n "$MC" ] || { echo "SKIP: no llvm-mc"; exit 0; }
# LLVM 18 does not know `vscclrm {..., vpr}`
skip_if_llvm_old "$MC"
n=0
for f in lob mve-scalar; do
    "$EMBCC" --target=thumbv8m.main-none-eabihf -mcpu=cortex-m55 -c "$here/$f.s" \
        -o "$out/$f.o" 2> "$out/$f.err" || fail "EmbCC refused $f.s: $(head -3 "$out/$f.err")"
    "$MC" -triple=thumbv8.1m.main -mattr=+mve.fp,+lob,+8msecext,+fp64,+d32 -filetype=obj \
        "$here/$f.s" -o "$out/$f.ref.o" || fail "llvm-mc refused $f.s"
    for o in $f $f.ref; do
        llvm-objcopy -O binary -j .text "$out/$o.o" "$out/$o.bin"
    done
    cmp -s "$out/$f.bin" "$out/$f.ref.bin" || {
        cmp -l "$out/$f.bin" "$out/$f.ref.bin" | head -4
        fail "$f.s: the bytes differ from llvm-mc's"; }
    n=$((n + $(grep -c '^	[a-z]' "$here/$f.s")))
done
echo "$n ARMv8.1-M statements encode as llvm-mc's"
"$EMBCC" --target=thumbv7m-none-eabi -c "$here/lob.s" -o "$out/x.o" 2> "$out/x.err" &&
    fail "csel was accepted on ARMv7-M"
grep -q "is an ARMv8.1-M instruction" "$out/x.err" || fail "refused on ARMv7-M, but not by name"
"$EMBCC" --target=thumbv7m-none-eabi -c "$here/mve-scalar.s" -o "$out/x.o" 2> "$out/x.err" &&
    fail "MVE was accepted on ARMv7-M"
grep -q "MVE" "$out/x.err" || fail "MVE refused on ARMv7-M, but not by name"
echo "on ARMv7-M, refused by name"
