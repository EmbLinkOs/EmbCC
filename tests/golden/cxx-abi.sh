#!/bin/sh
# EmbCC's C++ links with g++'s (D-013): cxx-abi/a.cc is compiled by embcc,
# cxx-abi/b.cc by the reference g++, and each calls the other through
# abi.h — namespaces, overloads over every builtin type, references,
# pointers and their qualifiers, repeated types (substitutions), function
# pointers, arrays, enums, nullptr_t, character types, varargs, a nested
# class, a class by value, std::, and a class whose constructors and
# destructor embcc defines being built and destroyed by g++'s code. A name
# mangled differently fails the link; a layout or convention that differs
# fails a check.
set -u
echo "TEST-MARKER cxx-abi"
. "$(dirname "$0")/../lib.sh"

if [ "$ARCH" = aarch64 ]; then
    REF=$AARCH64_REF_GXX NL=$AARCH64_NEWLIB
else
    REF=$X86_REF_GXX NL=$X86_NEWLIB
fi
GXX=$REF/bin/$TARGET-g++
if [ ! -x "$GXX" ]; then
    echo "skipped: no reference g++ at $REF (tools/build-ref-gxx.sh $TARGET)"
    exit 0
fi
src=$EMBCC_ROOT/tests/golden/cxx-abi
out=$EMBCC_ROOT/tests/golden/out/cxx-abi-$ARCH
mkdir -p "$out"
rm -f "$out"/*
"$EMBCC" --target="$TARGET" -I"$NL/include" -c "$src/a.cc" -o "$out/a.o" || {
    echo "embcc failed on a.cc"; exit 1; }
"$GXX" -std=c++20 -c "$src/b.cc" -o "$out/b.o" || { echo "g++ failed on b.cc"; exit 1; }
EMBCC_REF_GXX=$REF "$EMBCC_ROOT/tests/harness/$ARCH/link.sh" --cxx \
    -o "$out/abi" "$out/a.o" "$out/b.o" || { echo "link failed"; exit 1; }
res=$(t_run "$out/abi"); st=$?
printf '%s\n' "$res"
[ "$st" -eq 42 ] || { echo "exit $st, expected 42"; exit 1; }
