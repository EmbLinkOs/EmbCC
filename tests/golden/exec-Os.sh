#!/bin/sh
# Every exec/ program built -Os, against gcc's -- as regalloc-O2.sh does at
# -O2. -Os is not -O2 with smaller numbers: switches become compare trees
# instead of jump tables, constants sink into loops, loops are not split
# or unrolled, and the inliner's budget is smaller. None of that ran
# under a test on x86-64 or aarch64, and a cfg-clean bug that only those
# compare trees exposed -- a switch's case 0 deleted as unreachable --
# was found by fuzzing instead.
set -u
echo "TEST-MARKER exec-Os"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out_dir="tests/golden/out/exec-Os-$ARCH"
rm -rf "${out_dir:?}"; mkdir -p "$out_dir"

n=0
for c in tests/exec/*.c; do
    [ -e "$c" ] || continue
    name=$(basename "$c" .c)
    pinned_elsewhere "$c" && continue
    no_gcc_reference "$c" && continue
    "$EMBCC" --target="$TARGET" -Os -c "$c" -o "$out_dir/$name.o" || {
        echo "$name: embcc -Os failed to compile"; exit 1; }
    t_link "$out_dir/$name.embcc" "$out_dir/$name.o" || {
        echo "$name: link of -Os object failed"; exit 1; }
    t_gcc_c "$c" -std=c11 -o "$out_dir/$name.gcc.o" || {
        echo "$name: not valid C11 (needed for the gcc reference)"; exit 1; }
    t_link "$out_dir/$name.gcc" "$out_dir/$name.gcc.o" || {
        echo "$name: link of the gcc object failed"; exit 1; }
    out_a=$(t_run "$out_dir/$name.embcc"); a=$?
    out_b=$(t_run "$out_dir/$name.gcc"); b=$?
    if [ "$a" -ne "$b" ]; then
        echo "$name: embcc -Os exits $a, gcc exits $b — -Os miscompile"
        exit 1
    fi
    if [ "$out_a" != "$out_b" ]; then
        echo "$name: stdout differs at -Os:"
        printf 'embcc: %s\ngcc:   %s\n' "$out_a" "$out_b"
        exit 1
    fi
    n=$((n + 1))
done

echo "all $n exec programs agree with gcc when built -Os"
