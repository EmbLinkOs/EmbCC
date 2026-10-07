#!/bin/sh
# The LP64S calling convention, against clang ACROSS THE CALL.
#
# Each program pair is a caller and a callee in separate files. Each side
# is compiled by EmbCC or by clang (--target=loongarch64-unknown-elf
# -msoft-float, with its integrated assembler -- the reference, there
# being no LoongArch gcc here), the objects are linked by embld with the
# harness and lib/rt, and the image runs on QEMU's virt board. clang
# calling clang is the reference output; EmbCC calling EmbCC, EmbCC
# calling clang and clang calling EmbCC must print the same, with EmbCC's
# side at -O0 and at -O2. A backend that read LP64S consistently wrong
# would agree with itself all the way through the exec corpus; only a
# pairing with another compiler shows it.
#
# Three pairs: tests/golden/embedded-abi-*.c, shared with the ARMv7-M,
# RISC-V and MIPS suites (structs by value and returned, a composite
# straddling the last argument register, eight-byte scalars, variadics);
# tests/golden/embedded-abi128-*.c, RV64's 2*XLEN rules for __int128 and
# long double, which LoongArch shares; and tests/golden/loongarch-abi-*.c
# for what is particular to LP64S -- see loongarch-abi.h.
#
# clang's objects are its DEFAULT code model, medium: calls are
# pcaddu18i/jirl (R_LARCH_CALL36), another unit's global or function is
# reached through the GOT (R_LARCH_GOT_PC_HI20/LO12), and each carries an
# R_LARCH_RELAX. embld builds no GOT and rewrites those accesses, so this
# is EmbLD's test for foreign objects as well.
set -u
echo "TEST-MARKER loongarch-abi"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_LOONGARCH:-qemu-system-loongarch64}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
CLANG=${EMBCC_REF_CLANG_LOONGARCH:-clang}
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=loongarch64-unknown-elf -msoft-float \
        -fsyntax-only -x c /dev/null 2>/dev/null || {
    echo "skipped: no clang with a LoongArch target (set EMBCC_REF_CLANG_LOONGARCH)"
    exit 0; }

T=loongarch64-unknown-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/loongarch-abi
rm -rf "$out"; mkdir -p "$out"
export EMBCC_LOONGARCH_HARNESS="$PWD/$out"

for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/loongarch/$f.c" \
        -o "$out/$f.o" || { echo "the harness does not compile"; exit 1; }
done
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" || {
    echo "lib/rt does not build for $T"; exit 1; }

compile() {             # compile CC OPT SRC OBJ
    # -ffp-contract=off: clang fuses a*b+c into fmaf otherwise, a libm
    # call the image has no libm for (EmbCC never contracts)
    if [ "$1" = clang ]; then
        "$CLANG" --target=loongarch64-unknown-elf -msoft-float \
            -ffp-contract=off -ffreestanding -O1 -I tests/golden \
            -c "$3" -o "$4"
    else
        "$EMBCC" --target=$T "$2" -I tests/golden -c "$3" -o "$4"
    fi
}

run_pair() {            # run_pair PROG CALLER-CC CALLEE-CC OPT TAG
    compile "$2" "$4" "tests/golden/$1-caller.c" "$out/$5-caller.o" || {
        echo "$5: $2 could not compile $1-caller.c"; return 1; }
    compile "$3" "$4" "tests/golden/$1-callee.c" "$out/$5-callee.o" || {
        echo "$5: $3 could not compile $1-callee.c"; return 1; }
    EMBLD="$EMBLD" sh tests/harness/loongarch/link.sh "$out/$5.elf" \
        "$out/$5-caller.o" "$out/$5-callee.o" "$out/lib/librt.a" \
        > "$out/$5.lerr" 2>&1 || {
        echo "$5: embld could not link it:"; head -4 "$out/$5.lerr"
        return 1; }
    sh tests/harness/loongarch/run.sh "$out/$5.elf" > "$out/$5.txt" 2>&1
    grep -q '==END==' "$out/$5.txt" || {
        echo "$5: the image did not reach the end of main:"
        head -6 "$out/$5.txt"; return 1; }
    return 0
}

# The relocations clang's objects bring, which embld must have applied
# for the clang-clang image to have run at all: checked present, so that
# a change to clang's defaults cannot quietly stop testing them.
compile clang -O1 tests/golden/loongarch-abi-caller.c "$out/rel.o" || {
    echo "clang could not compile the caller"; exit 1; }
for r in R_LARCH_CALL36 R_LARCH_GOT_PC_HI20 R_LARCH_GOT_PC_LO12 \
         R_LARCH_RELAX; do
    llvm-readelf -r "$out/rel.o" 2>/dev/null | grep -q "$r" || {
        echo "clang's object carries no $r any more -- the foreign-object"
        echo "half of this test needs another way to provoke it"; exit 1; }
done

for prog in embedded-abi embedded-abi128 loongarch-abi; do
    run_pair $prog clang clang -O1 "$prog-cc" || exit 1
    for opt in -O0 -O2; do
        for pair in "embcc embcc ee" "embcc clang ec" "clang embcc ce"; do
            set -- $pair
            tag="$prog-$3$opt"
            run_pair $prog "$1" "$2" $opt "$tag" || exit 1
            diff -u "$out/$prog-cc.txt" "$out/$tag.txt" > "$out/$tag.diff" || {
                echo "$prog: $1 calling $2 at $opt disagrees with clang"
                echo "calling clang:"
                head -16 "$out/$tag.diff"; exit 1; }
        done
    done
    echo "$prog: EmbCC and clang call each other identically at -O0 and -O2"
done
echo "LP64S calls agree with clang's in both directions"
