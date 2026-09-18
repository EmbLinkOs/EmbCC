#!/bin/sh
# The C++ golden cross-check: every tests/cxx program is also built with the
# reference g++ (tools/build-ref-gxx.sh) for the same target, both are RUN on
# the harness, and they must exit alike and print the same thing — the
# stronger check next to run.sh's exit code, since each test prints a line
# per property it checks. Both link the same libstdc++, so a difference is
# EmbCC's.
set -u
echo "TEST-MARKER cxx-agrees-with-gxx"
. "$(dirname "$0")/../lib.sh"

if [ "$ARCH" = aarch64 ]; then
    REF=$AARCH64_REF_GXX NL=$AARCH64_NEWLIB
else
    REF=$X86_REF_GXX NL=$X86_NEWLIB
fi
GXX=$REF/bin/$TARGET-g++
if [ ! -x "$GXX" ] || [ ! -f "$REF/$TARGET/lib/libstdc++.a" ]; then
    echo "skipped: no reference g++ at $REF (tools/build-ref-gxx.sh $TARGET)"
    exit 0
fi

out_dir="tests/golden/out/cxx-agrees-$ARCH"
mkdir -p "$out_dir"
link="$EMBCC_ROOT/tests/harness/$ARCH/link.sh"

for cc in tests/cxx/*.cc; do
    [ -e "$cc" ] || continue
    name=$(basename "$cc" .cc)
    rm -f "$out_dir/$name".*
    "$EMBCC" --target="$TARGET" -I"$NL/include" -c "$cc" \
        -o "$out_dir/$name.embcc.o" || { echo "$name: embcc failed"; exit 1; }
    EMBCC_REF_GXX=$REF "$link" --cxx -o "$out_dir/$name.embcc" \
        "$out_dir/$name.embcc.o" || { echo "$name: link failed"; exit 1; }
    # Strict ISO C++20 (not gnu++20): EmbCC's C++ must stay standard C++.
    "$GXX" -std=c++20 -c "$cc" -o "$out_dir/$name.gxx.o" || {
        echo "$name: not valid C++20 for g++"; exit 1; }
    EMBCC_REF_GXX=$REF "$link" --cxx -o "$out_dir/$name.gxx" \
        "$out_dir/$name.gxx.o" || { echo "$name: link of g++ object failed"; exit 1; }
    out_a=$(t_run "$out_dir/$name.embcc"); a=$?
    out_b=$(t_run "$out_dir/$name.gxx"); b=$?
    if [ "$a" -ne "$b" ]; then
        echo "$name: embcc exits $a, g++ exits $b"
        exit 1
    fi
    if [ "$out_a" != "$out_b" ]; then
        echo "$name: output differs between embcc and g++ builds:"
        printf '%s\n' "$out_a" > "$out_dir/$name.embcc.out"
        printf '%s\n' "$out_b" > "$out_dir/$name.gxx.out"
        diff "$out_dir/$name.gxx.out" "$out_dir/$name.embcc.out"
        exit 1
    fi
    echo "$name: both exit $a, same output"
done
