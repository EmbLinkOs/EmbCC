#!/bin/sh
# LoongArch64's one- and two-byte atomics -- ll.w/sc.w loops on the word
# that holds the field -- on the virt board at -O0, -O1, -O2 and -Os:
# tests/golden/loongarch-atomics.c checks every operation at every place a
# field can sit in its word, the value returned, the field written, and
# that the neighbouring bytes are untouched. Then the loop itself, in the
# object: an ll.w, an sc.w, the dbar on each side and no byte-wide store
# to the atomic object (which would be a read-modify-write that is not
# atomic against the neighbours).
set -u
echo "TEST-MARKER loongarch-atomics"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_LOONGARCH:-qemu-system-loongarch64}
command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped: $QEMU not found"; exit 0; }
T=loongarch64-unknown-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/loongarch-atomics
rm -rf "$out"; mkdir -p "$out"
export EMBCC_LOONGARCH_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/loongarch/$f.c" -o "$out/$f.o" ||
        { echo "the harness does not compile"; exit 1; }
done
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c tests/golden/loongarch-atomics.c \
        -o "$out/a$opt.o" || { echo "$opt: does not compile"; exit 1; }
    EMBLD="$EMBLD" sh tests/harness/loongarch/link.sh "$out/a$opt.elf" \
        "$out/a$opt.o" > "$out/l$opt.txt" 2>&1 || {
        echo "$opt: does not link:"; head -3 "$out/l$opt.txt"; exit 1; }
    sh tests/harness/loongarch/run.sh "$out/a$opt.elf" > "$out/r$opt.txt"
    grep -q '==EXIT 42 ==' "$out/r$opt.txt" || {
        echo "$opt: the program failed:"; head -3 "$out/r$opt.txt"; exit 1; }
done
echo "one- and two-byte atomics are right on the board at -O0, -O1, -O2, -Os"

printf 'unsigned char c; unsigned char f(void) { return __atomic_fetch_add(&c, 3, 5); }\n' \
    > "$out/one.c"
"$EMBCC" --target=$T -O2 -c "$out/one.c" -o "$out/one.o" || exit 1
llvm-objdump -d "$out/one.o" > "$out/one.dis" 2>/dev/null || exit 0
grep -q 'll\.w' "$out/one.dis" && grep -q 'sc\.w' "$out/one.dis" &&
[ "$(grep -c 'dbar' "$out/one.dis")" -ge 2 ] && ! grep -q 'st\.b' "$out/one.dis" || {
    echo "a byte fetch-add is not an ll.w/sc.w loop between dbars:"
    cat "$out/one.dis"; exit 1; }
echo "a byte fetch-add is an ll.w/sc.w loop between dbars, with no byte store"
