#!/bin/sh
# libsupc++ — the C++ runtime's core: exceptions (__cxa_throw, the
# personality routine), RTTI and dynamic_cast, operator new/delete, guards,
# the fundamental types' typeinfo — compiled by EmbCC from GCC's sources
# (tools/build-libstdcxx.sh), and programs that lean on it linked against
# it: each must exit and print as g++'s build of it, with g++'s library,
# does.
set -u
echo "TEST-MARKER cxx-libsupcxx"
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
out=$EMBCC_ROOT/tests/golden/out/cxx-libsupcxx-$ARCH
rm -rf "$out"
mkdir -p "$out"
OUT=$out/lib EMBCC=$EMBCC "$EMBCC_ROOT/tools/build-libstdcxx.sh" "$TARGET" \
    libsupc++ > "$out/build.log" 2>&1 || {
    echo "libsupc++ did not compile:"; cat "$out/build.log"; exit 1; }
tail -2 "$out/build.log"
MINE=$out/lib/ref
ver=$(ls "$REF/$TARGET/include/c++" | head -1)
INC="$REF/$TARGET/include/c++/$ver"
link="$EMBCC_ROOT/tests/harness/$ARCH/link.sh"

for cc in tests/cxx/except.cc tests/cxx/rtti.cc tests/cxx/coroutines.cc \
          tests/libstdcxx/utilities.cc tests/libstdcxx/coroutine.cc; do
    name=$(basename "$cc" .cc)
    case $cc in
    tests/cxx/*) inc="-I$NL/include" gxxinc="-I$NL/include" ;;
    *) inc="-I$INC -I$INC/$TARGET -I$NL/include" gxxinc= ;;
    esac
    "$EMBCC" --target="$TARGET" $inc -c "$cc" -o "$out/$name.o" || {
        echo "$name: embcc failed"; exit 1; }
    EMBCC_REF_GXX=$MINE "$link" --cxx -o "$out/$name" "$out/$name.o" || {
        echo "$name: link with EmbCC's libsupc++ failed"; exit 1; }
    "$GXX" -std=c++20 $gxxinc -c "$cc" -o "$out/$name.g.o" || {
        echo "$name: g++ failed"; exit 1; }
    EMBCC_REF_GXX=$REF "$link" --cxx -o "$out/$name.g" "$out/$name.g.o" || {
        echo "$name: link of g++'s object failed"; exit 1; }
    a=$(t_run "$out/$name"; echo "exit $?")
    b=$(t_run "$out/$name.g"; echo "exit $?")
    if [ "$a" != "$b" ]; then
        echo "$name: EmbCC's libsupc++ ($(echo "$a" | tail -1)) differs from g++'s ($(echo "$b" | tail -1))"
        printf '%s\n' "$a" > "$out/$name.embcc.out"
        printf '%s\n' "$b" > "$out/$name.gxx.out"
        diff "$out/$name.gxx.out" "$out/$name.embcc.out"
        exit 1
    fi
    echo "$name: same with EmbCC's libsupc++ ($(echo "$a" | tail -1))"
done
