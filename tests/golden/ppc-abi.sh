#!/bin/sh
# The PowerPC EABI calling convention, against clang ACROSS THE CALL.
#
# Each program pair is a caller and a callee in separate files. Each side is
# compiled by EmbCC or by clang (--target=powerpc-none-eabi -mcpu=e500
# -mno-spe -msoft-float -mlong-double-64, with its integrated assembler --
# the reference, there being no PowerPC gcc here), the objects are linked
# by embld with the harness and lib/rt, and the image runs on QEMU's
# ppce500 board. clang calling clang is the reference output; EmbCC calling
# EmbCC, EmbCC calling clang and clang calling EmbCC must print the same,
# with EmbCC's side at -O0 and at -O2. A backend that read the convention
# consistently wrong would agree with itself all the way through the exec
# corpus; only a pairing with another compiler shows it.
#
# Three pairs: tests/golden/embedded-abi-*.c, shared with the ARMv7-M,
# RISC-V and MIPS suites (structs by value and returned, 8-byte scalars,
# variadics); tests/golden/mips-abi-*.c, whose shapes are portable C (soft
# float, narrow arguments, small composites in and out, a 36-byte struct, a
# far array element whose address's low half crosses 0x8000); and
# tests/golden/ppc-abi-*.c for what is particular to the EABI -- see
# ppc-abi.h: odd register pairs and the stack after r10, composites by
# reference to a copy the callee may write, composites of 1..8 bytes
# returned right-justified in r3:r4, _Complex, variadic long longs and
# structs, and a va_list record built by one compiler and walked by the
# other.
set -u
NAME=ppc-abi T=powerpc-none-eabi CT=powerpc-none-eabi
REF="-mcpu=e500 -mno-spe -msoft-float -mlong-double-64"
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_PPC:-qemu-system-ppc}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
CLANG=${EMBCC_REF_CLANG_PPC:-clang}
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=$CT $REF -fsyntax-only -x c /dev/null 2>/dev/null || {
    echo "skipped: no clang with a PowerPC target (set EMBCC_REF_CLANG_PPC)"
    exit 0; }

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out"
export EMBCC_PPC_HARNESS="$PWD/$out"

for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/ppc/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" || {
    echo "lib/rt does not build for $T"; exit 1; }
# clang copies a large struct argument with a call to memcpy
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" || {
    echo "lib/libc does not build for $T"; exit 1; }

compile() {             # compile CC OPT SRC OBJ
    if [ "$1" = clang ]; then
        "$CLANG" --target=$CT $REF -ffreestanding -O1 -I tests/golden \
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
    sh tests/harness/ppc/link.sh "$out/$5.elf" "$out/$5-caller.o" \
        "$out/$5-callee.o" "$out/lib/libc.a" "$out/lib/librt.a" \
        > "$out/$5.lerr" 2>&1 || {
        echo "$5: embld could not link it:"; head -4 "$out/$5.lerr"
        return 1; }
    sh tests/harness/ppc/run.sh "$out/$5.elf" > "$out/$5.txt" 2>&1
    grep -q '==END==' "$out/$5.txt" || {
        echo "$5: the image did not reach the end of main:"
        head -6 "$out/$5.txt"; return 1; }
    return 0
}

for prog in embedded-abi mips-abi ppc-abi; do
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
echo "PowerPC EABI calls ($T) agree with clang's in both directions"
