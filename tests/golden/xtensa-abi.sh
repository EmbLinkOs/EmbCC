#!/bin/sh
# The windowed calling convention, against Espressif's GCC ACROSS THE CALL.
#
# Each program pair is a caller and a callee in separate files. Each side
# is compiled by EmbCC or by Espressif's GCC generating for the de212 core
# the board emulates (tools/xtensa-ref-gcc.sh: its -mdynconfig plugin, so
# GCC's side carries no instruction the core lacks), the objects are
# linked by embld with the harness and lib/rt, and the image runs on QEMU's
# sim machine. GCC calling GCC is the reference output; EmbCC calling
# EmbCC, EmbCC calling GCC and GCC calling EmbCC must print the same, with
# EmbCC's side at -O0 and at -O2. A backend that read the ABI consistently
# wrong would agree with itself all the way through the exec corpus; only
# a pairing with the compiler ESP-IDF is built with shows it.
#
# Two pairs: tests/golden/embedded-abi-*.c, shared with the ARMv7-M, RISC-V
# and MIPS suites, and tests/golden/xtensa-abi-*.c for what is particular to
# the windowed ABI -- see xtensa-abi.h: the six argument words and what
# goes to the stack, results of up to 16 bytes in a2-a5, split _Complex
# arguments, narrow values, variadics across the register/stack boundary,
# and a va_list passed by value in both directions.
#
# GCC's objects carry R_XTENSA_SLOT0_OP on every branch and l32r, ASM_EXPAND
# hints and .xt.prop sections, which embld must apply, ignore and drop; so
# this is EmbLD's test for foreign objects as well. Skipped without that
# GCC (tools/hostpaths.sh: XTENSA_REF_GCC, XTENSA_REF_FLAGS).
set -u
echo "TEST-MARKER xtensa-abi"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_XTENSA:-qemu-system-xtensa}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
# Espressif's GCC for the de212 board (tools/xtensa-ref-gcc.sh, located by
# tools/hostpaths.sh), or nothing.
REFGCC=
if [ -x "$XTENSA_REF_GCC" ] &&
   "$XTENSA_REF_GCC" $XTENSA_REF_FLAGS -dM -E -x c /dev/null 2>/dev/null |
       grep -q '__XCHAL_HAVE_MUL32_HIGH 0'; then
    REFGCC=$XTENSA_REF_GCC
fi
[ -n "$REFGCC" ] || {
    echo "skipped: no Xtensa GCC for the de212 (tools/xtensa-ref-gcc.sh)"; exit 0; }

T=xtensa-none-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/xtensa-abi
rm -rf "$out"; mkdir -p "$out"
export EMBCC_XTENSA_HARNESS="$PWD/$out"

for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/xtensa/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" || {
    echo "lib/rt does not build for $T"; exit 1; }
# GCC copies a struct with memcpy
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" || {
    echo "lib/libc does not build for $T"; exit 1; }

compile() {             # compile CC OPT SRC OBJ
    if [ "$1" = gcc ]; then
        "$REFGCC" $XTENSA_REF_FLAGS -mtext-section-literals -ffreestanding -fno-builtin -O1 \
            -I tests/golden -c "$3" -o "$4"
    else
        "$EMBCC" --target=$T "$2" -I tests/golden -c "$3" -o "$4"
    fi
}

run_pair() {            # run_pair PROG CALLER-CC CALLEE-CC OPT TAG
    compile "$2" "$4" "tests/golden/$1-caller.c" "$out/$5-caller.o" || {
        echo "$5: $2 could not compile $1-caller.c"; return 1; }
    compile "$3" "$4" "tests/golden/$1-callee.c" "$out/$5-callee.o" || {
        echo "$5: $3 could not compile $1-callee.c"; return 1; }
    sh tests/harness/xtensa/link.sh "$out/$5.elf" "$out/$5-caller.o" \
        "$out/$5-callee.o" "$out/lib/libc.a" "$out/lib/librt.a" \
        > "$out/$5.lerr" 2>&1 || {
        echo "$5: embld could not link it:"; head -4 "$out/$5.lerr"
        return 1; }
    sh tests/harness/xtensa/run.sh "$out/$5.elf" > "$out/$5.txt" 2>&1
    grep -q '==END==' "$out/$5.txt" || {
        echo "$5: the image did not reach the end of main:"
        head -6 "$out/$5.txt"; return 1; }
    return 0
}

for prog in embedded-abi xtensa-abi; do
    run_pair $prog gcc gcc -O1 "$prog-gg" || exit 1
    for opt in -O0 -O2; do
        for pair in "embcc embcc ee" "embcc gcc eg" "gcc embcc ge"; do
            set -- $pair
            tag="$prog-$3$opt"
            run_pair $prog "$1" "$2" $opt "$tag" || exit 1
            diff -u "$out/$prog-gg.txt" "$out/$tag.txt" > "$out/$tag.diff" || {
                echo "$prog: $1 calling $2 at $opt disagrees with GCC"
                echo "calling GCC:"
                head -16 "$out/$tag.diff"; exit 1; }
        done
    done
    echo "$prog: EmbCC and GCC call each other identically at -O0 and -O2"
done
echo "windowed-ABI calls agree with Espressif GCC's in both directions"
