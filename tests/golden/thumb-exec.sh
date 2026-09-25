#!/bin/sh
# What the ARMv7-M backend COMPUTES, not just what it encodes (D-015).
#
# Each program is compiled by EmbCC, linked by embld into a firmware
# image, and run on QEMU's Cortex-M3 — then compiled by clang for the
# same triple, linked by the same embld, and run on the same board. The
# two outputs must agree.
#
# That shape matters: the reference travels through EmbCC's own linker
# and harness, so a difference is the COMPILER's and not a difference in
# how the image was built. It is the same discipline as
# agrees-with-gcc.sh, on a machine where nothing else here runs.
#
# Every wrong answer this backend has produced was found this way and by
# nothing else: an `and` emitted as `eor`, a folded immediate read as a
# register, and a fifth argument written over the first local all
# assemble, link and disassemble perfectly.
set -u
echo "TEST-MARKER thumb-exec"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP: $QEMU not found (set EMBCC_QEMU_ARM)"; exit 0; }
command -v "$CLANG" >/dev/null 2>&1 || {
    echo "SKIP: no reference compiler for thumbv7m (set EMBCC_REF_GCC_THUMB)"
    exit 0; }

T=thumbv7m-none-eabi
H=tests/harness/thumb
out=tests/golden/out/thumb-exec
rm -rf "$out"; mkdir -p "$out"

# The harness itself is built by EmbCC — the startup is C, because a
# Cortex-M fetches its initial SP and PC from the vector table in
# hardware and needs no assembler to begin.
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$H/$f.o" || {
        echo "the harness does not compile for $T"; exit 1; }
done

run_image() {           # run_image OBJ TAG -> $out/TAG.txt
    sh "$H/link.sh" "$out/$2.elf" "$1" || {
        echo "$2: embld could not link the image"; return 1; }
    sh "$H/run.sh" "$out/$2.elf" > "$out/$2.txt" 2>&1
    grep -q '==END==' "$out/$2.txt" || {
        echo "$2: the image did not reach the end of main:"
        sed -n '1,10p' "$out/$2.txt"
        return 1; }
    return 0
}

for src in tests/golden/thumb-stress.c; do
    base=$(basename "$src" .c)

    # The reference: the same source, the same linker, the same board.
    "$CLANG" -target $T -ffreestanding -Os -c "$src" -o "$out/$base-ref.o" || {
        echo "$base: the reference compiler could not compile it"; exit 1; }
    run_image "$out/$base-ref.o" "$base-ref" || exit 1

    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$src" -o "$out/$base$opt.o" || {
            echo "$base $opt: EmbCC could not compile it"; exit 1; }
        run_image "$out/$base$opt.o" "$base$opt" || exit 1
        if ! diff -u "$out/$base-ref.txt" "$out/$base$opt.txt" \
             > "$out/$base$opt.diff"; then
            echo "$base at $opt does not agree with the reference compiler:"
            head -20 "$out/$base$opt.diff"
            exit 1
        fi
    done
    echo "$base: EmbCC agrees with $CLANG at -O0, -O1, -O2 and -Os"
done

# 64-bit integers are checked against the HOST rather than against
# clang. `long long` arithmetic has one answer whatever the register
# width, so the machine this suite runs on is as good a reference — and
# a better one here, because clang for thumbv7m emits __aeabi_ldivmod
# for a 64-bit divide where EmbCC emits __divdi3, and the two cannot
# share a runtime.
"$EMBCC" --target=$T -Os -c lib/rt/int64.c -o "$out/int64.o" || {
    echo "the 64-bit runtime does not compile for $T"; exit 1; }
cc -std=c99 -w -o "$out/host64" tests/golden/thumb-int64.c \
   tests/harness/thumb/hostio.c || {
    echo "the 64-bit program does not compile for the host"; exit 1; }
"$out/host64" > "$out/int64-ref.txt" || {
    echo "the 64-bit program failed on the host"; exit 1; }

for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c tests/golden/thumb-int64.c \
             -o "$out/i64$opt.o" || {
        echo "$opt: the 64-bit program does not compile"; exit 1; }
    sh "$H/link.sh" "$out/i64$opt.elf" "$out/i64$opt.o" "$out/int64.o" || {
        echo "$opt: embld could not link the 64-bit image"; exit 1; }
    sh "$H/run.sh" "$out/i64$opt.elf" > "$out/i64$opt.txt" 2>&1
    grep -q '==END==' "$out/i64$opt.txt" || {
        echo "$opt: the 64-bit image did not reach the end of main:"
        sed -n '1,10p' "$out/i64$opt.txt"; exit 1; }
    if ! diff -u "$out/int64-ref.txt" "$out/i64$opt.txt" > "$out/i64$opt.diff"
    then
        echo "64-bit arithmetic at $opt does not agree with the host:"
        head -20 "$out/i64$opt.diff"
        exit 1
    fi
done
echo "thumb-int64: 64-bit arithmetic agrees with the host at four levels"

echo "ARMv7-M images build with embld and run on $QEMU"
