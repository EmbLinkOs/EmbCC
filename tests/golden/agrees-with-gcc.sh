#!/bin/sh
# The golden cross-check (tests/README): every exec/ program is also built
# with the target's gcc, both binaries are RUN, and they must exit with the
# same code and print the same output. This is what catches EmbCC accepting a
# different language than C or computing a different answer than the
# reference compiler — on whichever machine tests/run.sh --target= chose.
set -u
echo "TEST-MARKER agrees-with-gcc"
. "$(dirname "$0")/../lib.sh"

out_dir="tests/golden/out/agrees-$ARCH"
mkdir -p "$out_dir"

for c in tests/exec/*.c tests/exec/$ARCH/*.c; do
    [ -e "$c" ] || continue
    name=$(basename "$c" .c)
    pinned_elsewhere "$c" && { echo "$name: pinned to another target, skipped"; continue; }
    no_gcc_reference "$c" && continue
    rm -f "$out_dir/$name".*
    "$EMBCC" --target="$TARGET" -c "$c" -o "$out_dir/$name.embcc.o" || {
        echo "$name: embcc failed"; exit 1; }
    t_link "$out_dir/$name.embcc" "$out_dir/$name.embcc.o" || {
        echo "$name: link of embcc object failed"; exit 1; }
    # Strict ISO C11 (not gnu11): EmbCC's language must stay a subset of
    # standard C, or comparing it against gcc proves nothing. C11 rather than
    # C99 because the corpus uses _Static_assert, _Generic, _Alignas and the
    # u8/u/U literals — and the EmbLinkOS kernel is C11.
    t_gcc_c "$c" -std=c11 -o "$out_dir/$name.gcc.o" || {
        echo "$name: not valid C11 — EmbCC's language must stay a strict"
        echo "subset of C, or golden comparisons are impossible"
        exit 1; }
    t_link "$out_dir/$name.gcc" "$out_dir/$name.gcc.o" || {
        echo "$name: link of gcc object failed"; exit 1; }
    out_a=$(t_run "$out_dir/$name.embcc"); a=$?
    out_b=$(t_run "$out_dir/$name.gcc"); b=$?
    if [ "$a" -ne "$b" ]; then
        echo "$name: embcc exits $a, gcc exits $b"
        exit 1
    fi
    if [ "$out_a" != "$out_b" ]; then
        echo "$name: stdout differs between embcc and gcc builds:"
        printf 'embcc: %s\ngcc:   %s\n' "$out_a" "$out_b"
        exit 1
    fi
    echo "$name: both exit $a, same output"
done
