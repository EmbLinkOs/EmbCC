#!/bin/sh
# libstdc++ itself: each tests/libstdcxx program compiled by EmbCC against
# the reference g++'s libstdc++ headers, linked with its libstdc++.a (the
# library g++ built — EmbCC's objects must be ABI-compatible with it:
# mangling, layout, calling convention, exceptions), RUN on the harness,
# and required to exit and print as g++'s build of the same source does.
set -u
echo "TEST-MARKER cxx-libstdcxx"
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
ver=$(ls "$REF/$TARGET/include/c++" | head -1)
INC="$REF/$TARGET/include/c++/$ver"

out_dir="tests/golden/out/cxx-libstdcxx-$ARCH"
mkdir -p "$out_dir"
link="$EMBCC_ROOT/tests/harness/$ARCH/link.sh"

for cc in tests/libstdcxx/*.cc; do
    [ -e "$cc" ] || continue
    name=$(basename "$cc" .cc)
    rm -f "$out_dir/$name".*
    "$EMBCC" --target="$TARGET" -I"$INC" -I"$INC/$TARGET" -I"$NL/include" \
        -c "$cc" -o "$out_dir/$name.embcc.o" || {
        echo "$name: embcc failed"; exit 1; }
    EMBCC_REF_GXX=$REF "$link" --cxx -o "$out_dir/$name.embcc" \
        "$out_dir/$name.embcc.o" || { echo "$name: link failed"; exit 1; }
    "$GXX" -std=c++20 -c "$cc" -o "$out_dir/$name.gxx.o" || {
        echo "$name: g++ failed"; exit 1; }
    EMBCC_REF_GXX=$REF "$link" --cxx -o "$out_dir/$name.gxx" \
        "$out_dir/$name.gxx.o" || { echo "$name: link of g++ object failed"; exit 1; }
    out_a=$(t_run "$out_dir/$name.embcc"); a=$?
    out_b=$(t_run "$out_dir/$name.gxx"); b=$?
    if [ "$a" -ne "$b" ] || [ "$out_a" != "$out_b" ]; then
        echo "$name: embcc exits $a, g++ exits $b"
        printf '%s\n' "$out_a" > "$out_dir/$name.embcc.out"
        printf '%s\n' "$out_b" > "$out_dir/$name.gxx.out"
        diff "$out_dir/$name.gxx.out" "$out_dir/$name.embcc.out"
        exit 1
    fi
    echo "$name: both exit $a, same output"
done
