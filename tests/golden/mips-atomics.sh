#!/bin/sh
# MIPS atomics narrower than a word: MIPS32 and MIPS64, both byte orders.
#
# ll/sc (and lld/scd at MIPS64) are a word and a doubleword, and nothing
# smaller, so a one- or two-byte atomic is an ll/sc loop on the aligned
# word around it that rewrites only its lane, old ^ ((new ^ old) & mask) --
# atomic against the neighbouring bytes too, since a store to any of them
# breaks the link. That is GCC's and LLVM's lowering, and RISC-V's
# (riscv-atomics.sh), whose tests/golden/riscv-atomics/subword.c this runs:
# every operation on every lane of one word, the whole word printed after
# each, so a clobbered neighbour shows.
#
# THE LANE DEPENDS ON THE BYTE ORDER. Big-endian, the byte at address
# offset 0 is the word's top byte, so the shift is ((a & 3) ^ (4 - size)) * 8,
# not (a & 3) * 8. The little-endian boards are compared with the host;
# the host cannot referee a big-endian board, whose printed words differ,
# so there the referee is clang's build of the same program on the same
# board (skipped without clang).
set -u
echo "TEST-MARKER mips-atomics"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/mips-atomics
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
SUB=tests/golden/riscv-atomics/subword.c

# ---- the macros claim exactly the widths that exist -------------------
for t in mipsel-none-elf mips-none-elf mips64el-none-elf mips64-none-elf; do
    m=$("$EMBCC" --target=$t --dump-predef 2>/dev/null)
    for w in 1 2 4; do
        echo "$m" | grep -q "SYNC_COMPARE_AND_SWAP_$w\$\|SYNC_COMPARE_AND_SWAP_$w " || {
            echo "$t: a $w-byte compare-and-swap is not claimed, and the
            backend has one"; exit 1; }
    done
    case $t in
    mips64*) echo "$m" | grep -q 'SYNC_COMPARE_AND_SWAP_8' || {
                 echo "$t: an eight-byte compare-and-swap is not claimed, and
                 lld/scd exist there"; exit 1; } ;;
    *)       echo "$m" | grep -q 'SYNC_COMPARE_AND_SWAP_8' && {
                 echo "$t: claims an eight-byte compare-and-swap; lld/scd are
                 MIPS64-only"; exit 1; } ;;
    esac
    echo "$m" | grep -q 'SYNC_COMPARE_AND_SWAP_16' && {
        echo "$t: claims a sixteen-byte compare-and-swap"; exit 1; }
done
echo "MIPS32 and MIPS64 claim exactly the compare-and-swap sizes there are"

# ---- a sub-word atomic, on the board ----------------------------------
HOSTCC=${HOSTCC:-cc}
"$HOSTCC" -std=c99 -w -O2 -o "$out/sub-host" $SUB tests/harness/thumb/hostio.c ||
    { echo "the host does not build subword.c"; exit 1; }
"$out/sub-host" > "$out/want-le.txt"

S=$out/s; mkdir -p "$S"
# run TRIPLE HARNESS QEMU WANT: link $S/sub.o with the harness and run it
board() {
    sh tests/harness/$2/link.sh "$S/sub.elf" "$S/sub.o" ||
        { echo "$1: subword.c did not link"; exit 1; }
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
        sh tests/harness/$2/run.sh "$S/sub.elf" 2>/dev/null |
        tr -d '\r' | sed -n '1,/^DONE/p' > "$out/got.txt"
}
ran=
for row in "mipsel-none-elf mips le qemu-system-mipsel x x" \
           "mips-none-elf mips be qemu-system-mips mips-unknown-elf mips32r2" \
           "mips64el-none-elf mips64 le qemu-system-mips64el x x" \
           "mips64-none-elf mips64 be qemu-system-mips64 mips64-unknown-elf mips64r2"; do
    set -- $row
    t=$1 h=$2 e=$3 q=$4 ct=$5 cpu=$6
    command -v "$q" >/dev/null 2>&1 || { echo "(SKIP: no $q for $t)"; continue; }
    for f in boot io; do
        "$EMBCC" --target=$t -O1 -c tests/harness/mips/$f.c -o "$S/$f.o" ||
            { echo "$t: the harness did not compile"; exit 1; }
    done
    export EMBCC_MIPS_HARNESS="$S" EMBCC_MIPS64_HARNESS="$S"
    want=$out/want-le.txt
    if [ "$e" = be ]; then
        command -v clang >/dev/null 2>&1 || {
            echo "(SKIP: no clang to referee big-endian $t)"; continue; }
        abi=; [ "$h" = mips64 ] && abi=-mabi=64
        clang --target=$ct -mcpu=$cpu $abi -msoft-float -mno-abicalls -fno-pic \
            -G0 -ffreestanding -fno-builtin -O2 -c $SUB -o "$S/sub.o" 2>/dev/null || {
            echo "(SKIP: clang does not build subword.c for $ct)"; continue; }
        board "$t (clang)" $h
        want=$out/want-$t.txt
        cp "$out/got.txt" "$want"
        grep -q '^DONE' "$want" || {
            echo "$t: clang's build of subword.c did not finish on the board"
            exit 1; }
    fi
    for O in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$t $O -c $SUB -o "$S/sub.o" 2> "$out/sub.err" || {
            echo "$t $O: subword.c did not compile:"; head -3 "$out/sub.err"
            exit 1; }
        board "$t $O" $h
        cmp -s "$want" "$out/got.txt" || {
            echo "$t $O: the one- and two-byte atomics differ from the referee:"
            diff "$want" "$out/got.txt" | head -8; exit 1; }
    done
    ran="$ran $t"
done
echo "one- and two-byte atomics on every lane of a word, at every level, on:$ran"
