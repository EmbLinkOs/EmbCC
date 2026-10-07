#!/bin/sh
# PowerPC atomics narrower than a word.
#
# lwarx/stwcx. are a word, and Book E (the e500 the target is) has no
# lbarx/lharx, so a one- or two-byte atomic is a lwarx/stwcx. loop on the
# aligned word around it that rewrites only its lane,
# old ^ ((new ^ old) & mask) -- atomic against the neighbouring bytes too,
# since a store to any of them loses the reservation. That is GCC's and
# LLVM's lowering, and RISC-V's and MIPS's (riscv-atomics.sh,
# mips-atomics.sh), whose tests/golden/riscv-atomics/subword.c this runs:
# every operation on every lane of one word, the whole word printed after
# each, so a clobbered neighbour shows.
#
# THE LANE DEPENDS ON THE BYTE ORDER. PowerPC is big-endian: the byte at
# address offset 0 is the word's top byte, so the shift is
# ((a & 3) ^ (4 - size)) * 8. The host cannot referee a big-endian board,
# whose printed words differ, so the referee is clang's build of the same
# program on the same board (skipped without clang).
set -u
echo "TEST-MARKER ppc-atomics"
. "$(dirname "$0")/../lib.sh"

T=powerpc-none-eabi
QEMU=${EMBCC_QEMU_PPC:-qemu-system-ppc}
out=tests/golden/out/ppc-atomics
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
    echo "claims an eight-byte compare-and-swap; 32-bit PowerPC has none"
    exit 1; }
echo "PowerPC claims exactly the one-, two- and four-byte compare-and-swaps"

# ---- the loop in the object -------------------------------------------
printf 'char c; int f(void){ return __atomic_fetch_add(&c, 1, 5); }\n' > "$out/one.c"
"$EMBCC" --target=$T -O2 -c "$out/one.c" -o "$out/one.o" ||
    { echo "a one-byte fetch-and-add did not compile"; exit 1; }
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
if "$OD" -d "$out/one.o" > "$out/one.dis" 2>/dev/null; then
    for insn in lwarx stwcx. slw srw; do
        grep -q "[[:space:]]$insn[[:space:]]" "$out/one.dis" || {
            echo "a one-byte fetch-and-add has no $insn:"; cat "$out/one.dis"
            exit 1; }
    done
    echo "a one-byte fetch-and-add is a lwarx/stwcx. loop on the word"
else
    echo "(no llvm-objdump: the object is not read)"
fi

# ---- a sub-word atomic, on the board, against clang -------------------
command -v "$QEMU" >/dev/null 2>&1 || { echo "(SKIP: no $QEMU)"; exit 0; }
CLANG=${EMBCC_REF_CLANG_PPC:-clang}
command -v "$CLANG" >/dev/null 2>&1 || {
    echo "(SKIP: no clang to referee the big-endian board)"; exit 0; }
S=$out/s; mkdir -p "$S"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c tests/harness/ppc/$f.c -o "$S/$f.o" ||
        { echo "the harness did not compile"; exit 1; }
done
export EMBCC_PPC_HARNESS="$S"
board() {
    sh tests/harness/ppc/link.sh "$S/sub.elf" "$S/sub.o" ||
        { echo "$1: subword.c did not link"; exit 1; }
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
        sh tests/harness/ppc/run.sh "$S/sub.elf" 2>/dev/null |
        tr -d '\r' | sed -n '1,/^DONE/p' > "$out/got.txt"
}
"$CLANG" --target=$T -mcpu=e500 -mno-spe -msoft-float -mlong-double-64 \
    -ffreestanding -fno-builtin -O2 -c $SUB -o "$S/sub.o" 2>/dev/null || {
    echo "(SKIP: clang does not build subword.c for $T)"; exit 0; }
board "clang"
want=$out/want.txt
cp "$out/got.txt" "$want"
grep -q '^DONE' "$want" || {
    echo "clang's build of subword.c did not finish on the board"; exit 1; }
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c $SUB -o "$S/sub.o" 2> "$out/sub.err" || {
        echo "$O: subword.c did not compile:"; head -3 "$out/sub.err"; exit 1; }
    board "$O"
    cmp -s "$want" "$out/got.txt" || {
        echo "$O: the one- and two-byte atomics differ from clang's:"
        diff "$want" "$out/got.txt" | head -8; exit 1; }
done
echo "one- and two-byte atomics on every lane of a word, at -O0/-O1/-O2/-Os, equal clang's on the ppce500 board"
