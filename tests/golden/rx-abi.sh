#!/bin/sh
# GCC's RX calling convention, ACROSS THE CALL, against GCC itself.
#
# Each program pair is a caller and a callee in separate files, each side
# compiled by EmbCC or by rx-elf-gcc (-nofpu: GCC's default configuration
# less the FPU, which is EmbCC's), linked by embld with the harness and
# lib/rt, and run on QEMU's gdbsim board. GCC calling GCC is the reference
# output; EmbCC calling EmbCC, EmbCC calling GCC and GCC calling EmbCC must
# print the same, with EmbCC's side at -O0 and -O2. A backend that read
# the convention consistently wrong would agree with itself all the way
# through the exec corpus (a mutant that stopped stacked arguments
# advancing the register count did); only a pairing with another compiler
# shows it.
#
# Two pairs: tests/golden/embedded-abi-*.c, shared with the other embedded
# suites, and tests/golden/rx-abi-*.c for what is particular to RX -- see
# rx-abi.h. GCC's objects name their sections the Renesas way (P, D_1,
# B_1, ...), which embld must lay out with EmbCC's .text and .data, so
# this is EmbLD's test for foreign RX objects as well.
#
# rx-elf-gcc is not part of any toolchain this repository assumes: without
# one (EMBCC_RX_GCC, or rx-elf-gcc on PATH) the test says so and skips.
set -u
echo "TEST-MARKER rx-abi"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_RX:-qemu-system-rx}
command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped: $QEMU not found"; exit 0; }
GCC=${EMBCC_RX_GCC:-rx-elf-gcc}
command -v "$GCC" >/dev/null 2>&1 || {
    echo "skipped: no rx-elf-gcc to referee the convention (set EMBCC_RX_GCC)"
    exit 0; }
export EMBCC_OBJCOPY=${EMBCC_OBJCOPY:-llvm-objcopy}
command -v "$EMBCC_OBJCOPY" >/dev/null 2>&1 || {
    echo "skipped: $EMBCC_OBJCOPY not found"; exit 0; }

T=rx-none-elf
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/rx-abi
rm -rf "$out"; mkdir -p "$out"
export EMBCC_RX_HARNESS="$PWD/$out"

for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/rx/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" || {
    echo "lib/rt does not build for $T"; exit 1; }

compile() {             # compile CC OPT SRC OBJ
    if [ "$1" = gcc ]; then
        "$GCC" -nofpu -O1 -ffreestanding -w -I tests/golden -c "$3" -o "$4"
    else
        "$EMBCC" --target=$T "$2" -I tests/golden -c "$3" -o "$4"
    fi
}

run_pair() {            # run_pair PROG CALLER-CC CALLEE-CC OPT TAG
    compile "$2" "$4" "tests/golden/$1-caller.c" "$out/$5-caller.o" || {
        echo "$5: $2 could not compile $1-caller.c"; return 1; }
    compile "$3" "$4" "tests/golden/$1-callee.c" "$out/$5-callee.o" || {
        echo "$5: $3 could not compile $1-callee.c"; return 1; }
    sh tests/harness/rx/link.sh "$out/$5.elf" "$out/$5-caller.o" \
        "$out/$5-callee.o" "$out/lib/librt.a" > "$out/$5.lerr" 2>&1 || {
        echo "$5: embld could not link it:"; head -4 "$out/$5.lerr"
        return 1; }
    sh tests/harness/rx/run.sh "$out/$5.bin" > "$out/$5.txt" 2>&1
    grep -q '==END==' "$out/$5.txt" || {
        echo "$5: the image did not reach the end of main:"
        head -6 "$out/$5.txt"; return 1; }
    return 0
}

for prog in embedded-abi rx-abi; do
    run_pair $prog gcc gcc -O1 "$prog-gg" || exit 1
    for opt in -O0 -O2; do
        for pair in "embcc embcc ee" "embcc gcc eg" "gcc embcc ge"; do
            # shellcheck disable=SC2086
            set -- $pair
            tag="$prog-$3$opt"
            run_pair $prog "$1" "$2" $opt "$tag" || exit 1
            diff -u "$out/$prog-gg.txt" "$out/$tag.txt" > "$out/$tag.diff" || {
                echo "$prog: $1 calling $2 at $opt disagrees with GCC calling GCC:"
                head -16 "$out/$tag.diff"; exit 1; }
        done
    done
    echo "$prog: EmbCC and rx-elf-gcc call each other identically at -O0 and -O2"
done
echo "RX calls agree with GCC's in both directions"
