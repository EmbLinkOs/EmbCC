#!/bin/sh
# The SPARC calling convention, against clang ACROSS THE CALL.
#
# Each program pair is a caller and a callee in separate files. Each side
# is compiled by EmbCC or by clang (--target=sparc-none-elf -mcpu=leon3
# -msoft-float, with its integrated assembler -- the reference, there being
# no SPARC gcc here), the objects are linked by embld with the harness and
# lib/rt, and the image runs on QEMU's leon3_generic board. clang calling
# clang is the reference output; EmbCC calling EmbCC, EmbCC calling clang
# and clang calling EmbCC must print the same, with EmbCC's side at -O0
# and at -O2. A backend that read the convention consistently wrong would
# agree with itself all the way through the exec corpus; only a pairing
# with another compiler shows it.
#
# Two pairs: tests/golden/embedded-abi-*.c, shared with the ARMv7-M,
# RISC-V and MIPS suites, and tests/golden/sparc-abi-*.c for what is
# particular to SPARC -- see sparc-abi.h: unpadded argument words with a
# double or long long straddling %o5 and the stack, every aggregate and a
# long double by reference to the caller's copy, the struct-return word at
# %sp+64 and the `unimp` after the call, _Complex results in registers,
# narrow values, variadic doubles, long longs and structs, a far address
# and function pointers. The register windows are crossed both ways in
# every call: a clang function's save and restore against EmbCC's.
set -u
NAME=sparc-abi T=sparc-none-elf CT=sparc-none-elf
QEMU=${EMBCC_QEMU_SPARC:-qemu-system-sparc}
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
CLANG=${EMBCC_REF_CLANG_SPARC:-clang}
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=$CT -mcpu=leon3 -msoft-float \
        -fsyntax-only -x c /dev/null 2>/dev/null || {
    echo "skipped: no clang with a SPARC target (set EMBCC_REF_CLANG_SPARC)"
    exit 0; }

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out"
export EMBCC_SPARC_HARNESS="$PWD/$out"

for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/sparc/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" || {
    echo "lib/rt does not build for $T"; exit 1; }
# lib/libc for the memcpy clang's struct copies call
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" || {
    echo "lib/libc does not build for $T"; exit 1; }

compile() {             # compile CC OPT SRC OBJ
    if [ "$1" = clang ]; then
        "$CLANG" --target=$CT -mcpu=leon3 -msoft-float \
            -ffreestanding -O1 -I tests/golden -c "$3" -o "$4"
    else
        "$EMBCC" --target=$T "$2" -I tests/golden -c "$3" -o "$4"
    fi
}

run_pair() {            # run_pair PROG CALLER-CC CALLEE-CC OPT TAG
    compile "$2" "$4" "tests/golden/$1-caller.c" "$out/$5-caller.o" || {
        echo "$5: $2 could not compile $1-caller.c"; return 1; }
    compile "$3" "$4" "tests/golden/$1-callee.c" "$out/$5-callee.o" || {
        echo "$5: $3 could not compile $1-callee.c"; return 1; }
    sh tests/harness/sparc/link.sh "$out/$5.elf" "$out/$5-caller.o" \
        "$out/$5-callee.o" "$out/lib/libc.a" "$out/lib/librt.a" > "$out/$5.lerr" 2>&1 || {
        echo "$5: embld could not link it:"; head -4 "$out/$5.lerr"
        return 1; }
    sh tests/harness/sparc/run.sh "$out/$5.elf" > "$out/$5.txt" 2>&1
    grep -q '==END==' "$out/$5.txt" || {
        echo "$5: the image did not reach the end of main:"
        head -6 "$out/$5.txt"; return 1; }
    return 0
}

for prog in embedded-abi sparc-abi; do
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
echo "SPARC calls ($T) agree with clang's in both directions"
