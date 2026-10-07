#!/bin/sh
# SPARC (LEON3) atomics narrower than a word.
#
# casa and swap are a word, so a one- or two-byte atomic is a casa loop on
# the aligned word around it that rewrites only its lane,
# old ^ ((new ^ old) & mask) -- atomic against the neighbouring bytes too,
# since a store to any of them between the load and the casa makes the
# casa fail and the loop go round. A compare-and-swap compares the lane,
# not the word, so a neighbour's change goes round instead of failing it.
# That is GCC's and LLVM's lowering, and this runs RISC-V's and MIPS's
# program for it, tests/golden/riscv-atomics/subword.c: every operation on
# every lane of one word, the whole word printed after each, so a
# clobbered neighbour shows.
#
# THE LANE DEPENDS ON THE BYTE ORDER. SPARC is big-endian: the byte at
# address offset 0 is the word's top byte, so the shift is
# ((a & 3) ^ (4 - size)) * 8. The host cannot referee a big-endian board,
# whose printed words differ, so the referee is clang's build of the same
# program (clang makes it a casa loop too) on the same board, skipped
# without clang.
#
# (No __GCC_HAVE_SYNC_COMPARE_AND_SWAP_N is checked: the table is clang's
# for -mcpu=leon3, which defines none, tools/gen-predef.sh.)
set -u
echo "TEST-MARKER sparc-atomics"
. "$(dirname "$0")/../lib.sh"

T=sparc-none-elf
QEMU=${EMBCC_QEMU_SPARC:-qemu-system-sparc}
out=tests/golden/out/sparc-atomics
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
SUB=tests/golden/riscv-atomics/subword.c

# ---- the loop in the object -------------------------------------------
printf 'char c; int f(void){ return __atomic_fetch_add(&c, 1, 5); }\n' > "$out/one.c"
"$EMBCC" --target=$T -O2 -c "$out/one.c" -o "$out/one.o" ||
    { echo "a one-byte fetch-and-add did not compile"; exit 1; }
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
if "$OD" -d --mcpu=leon3 "$out/one.o" > "$out/one.dis" 2>/dev/null; then
    for insn in casa sll srl; do
        grep -q "[[:space:]]$insn[[:space:]]" "$out/one.dis" || {
            echo "a one-byte fetch-and-add has no $insn:"; cat "$out/one.dis"
            exit 1; }
    done
    echo "a one-byte fetch-and-add is a casa loop on the word"
else
    echo "(no llvm-objdump: the object is not read)"
fi

# ---- a sub-word atomic, on the board, against clang -------------------
command -v "$QEMU" >/dev/null 2>&1 || { echo "(SKIP: no $QEMU)"; exit 0; }
CLANG=${EMBCC_REF_CLANG_SPARC:-clang}
command -v "$CLANG" >/dev/null 2>&1 || {
    echo "(SKIP: no clang to referee the big-endian board)"; exit 0; }
S=$out/s; mkdir -p "$S"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c tests/harness/sparc/$f.c -o "$S/$f.o" ||
        { echo "the harness did not compile"; exit 1; }
done
export EMBCC_SPARC_HARNESS="$S"
board() {
    sh tests/harness/sparc/link.sh "$S/sub.elf" "$S/sub.o" ||
        { echo "$1: subword.c did not link"; exit 1; }
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
        sh tests/harness/sparc/run.sh "$S/sub.elf" 2>/dev/null |
        tr -d '\r' | sed -n '1,/^DONE/p' > "$out/got.txt"
}
"$CLANG" --target=$T -mcpu=leon3 -msoft-float -ffreestanding -fno-builtin \
    -O2 -c $SUB -o "$S/sub.o" 2>/dev/null || {
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
echo "one- and two-byte atomics on every lane of a word, at -O0/-O1/-O2/-Os, equal clang's on the leon3_generic board"
