#!/bin/sh
# The C++ suites wholly on EmbCC's library: libstdc++ and libsupc++ built
# from GCC's sources by EmbCC (tools/build-libstdcxx.sh — every object of
# both archives EmbCC's), then every program of tests/libstdcxx and
# tests/cxx compiled by EmbCC and linked with THAT library, run on the
# harness, and required to exit and print as g++'s build of the same
# source, with g++'s library, does.
#
# Building the library takes minutes, so this is opt-in: `make
# test-libstdcxx` (both targets), or EMBCC_LIBSTDCXX=1. EMBCC_LIBSTDCXX_LIB
# names a library already built by this EmbCC (tools/build-libstdcxx.sh's
# OUT) to use instead of building one.
set -u
echo "TEST-MARKER cxx-libstdcxx-embcc"
[ "${EMBCC_LIBSTDCXX:-}" = 1 ] || {
    echo "skipped: building libstdc++ is opt-in (make test-libstdcxx)"; exit 0; }
. "$(dirname "$0")/../lib.sh"

if [ "$ARCH" = aarch64 ]; then
    REF=$AARCH64_REF_GXX NL=$AARCH64_NEWLIB
else
    REF=$X86_REF_GXX NL=$X86_NEWLIB
fi
GXX=$REF/bin/$TARGET-g++
BUILD=${LIBSTDCXX_BUILD:-$HOME/cross/src/build-gcc-cxx-$TARGET/$TARGET/libstdc++-v3}
if [ ! -x "$GXX" ] || [ ! -d "$BUILD" ] ||
   [ ! -d "${GCC_SRC:-$HOME/cross/src/gcc-16.2.0}/libstdc++-v3" ]; then
    echo "skipped: no reference g++, GCC sources or configured libstdc++ build"
    exit 0
fi
out=$EMBCC_ROOT/tests/golden/out/cxx-libstdcxx-embcc-$ARCH
mkdir -p "$out"
if [ -n "${EMBCC_LIBSTDCXX_LIB:-}" ]; then
    lib=$EMBCC_LIBSTDCXX_LIB
else
    lib=$out/lib
    rm -rf "$lib"
    OUT=$lib EMBCC=$EMBCC "$EMBCC_ROOT/tools/build-libstdcxx.sh" "$TARGET" \
        > "$out/build.log" 2>&1 || {
        echo "libstdc++ did not build:"; tail -20 "$out/build.log"; exit 1; }
    # every object EmbCC's: a source it could not compile leaves g++'s
    # object in the archive, which would prove nothing
    grep -q " 0 failed" "$out/build.log" || {
        echo "libstdc++: not every source compiled:"
        grep -v "^ok" "$out/build.log" | tail -20; exit 1; }
    tail -2 "$out/build.log" | head -1
fi
MINE=$lib/ref
ver=$(ls "$REF/$TARGET/include/c++" | head -1)
INC="$REF/$TARGET/include/c++/$ver"
link="$EMBCC_ROOT/tests/harness/$ARCH/link.sh"

n=0
for cc in tests/libstdcxx/*.cc tests/cxx/*.cc; do
    [ -e "$cc" ] || continue
    name=$(basename "$cc" .cc)
    case $cc in
    tests/cxx/*) inc="-I$NL/include" gxxinc="-I$NL/include" ;;
    *) inc="-I$INC -I$INC/$TARGET -I$NL/include" gxxinc="" ;;
    esac
    rm -f "$out/$name".*
    # shellcheck disable=SC2086
    "$EMBCC" --target="$TARGET" $inc -c "$cc" -o "$out/$name.embcc.o" || {
        echo "$name: embcc failed"; exit 1; }
    EMBCC_REF_GXX=$MINE "$link" --cxx -o "$out/$name.embcc" \
        "$out/$name.embcc.o" || { echo "$name: link failed"; exit 1; }
    # shellcheck disable=SC2086
    "$GXX" -std=c++20 $gxxinc -c "$cc" -o "$out/$name.gxx.o" || {
        echo "$name: g++ failed"; exit 1; }
    EMBCC_REF_GXX=$REF "$link" --cxx -o "$out/$name.gxx" \
        "$out/$name.gxx.o" || { echo "$name: link of g++ object failed"; exit 1; }
    out_a=$(t_run "$out/$name.embcc"); a=$?
    out_b=$(t_run "$out/$name.gxx"); b=$?
    if [ "$a" -ne "$b" ] || [ "$out_a" != "$out_b" ]; then
        echo "$name: embcc (EmbCC's library) exits $a, g++ exits $b"
        printf '%s\n' "$out_a" > "$out/$name.embcc.out"
        printf '%s\n' "$out_b" > "$out/$name.gxx.out"
        diff "$out/$name.gxx.out" "$out/$name.embcc.out"
        exit 1
    fi
    n=$((n + 1))
done
echo "$n programs on EmbCC's libstdc++ ($ARCH): each exits and prints as"
echo "g++'s build with g++'s library does"
