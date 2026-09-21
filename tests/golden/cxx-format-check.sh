#!/bin/sh
# std::format's strings are checked when the program is compiled:
# basic_format_string's constructor is consteval, so each call's format
# string runs through libstdc++'s scanner (a class with virtual functions,
# format specs in bit-fields) in the constant evaluator — an ill-formed one
# is an error, as g++ makes it. Each case is compiled by both: they must
# agree on which strings are ill-formed.
set -u
echo "TEST-MARKER cxx-format-check"
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
ver=$(ls "$REF/$TARGET/include/c++" | head -1)
INC="$REF/$TARGET/include/c++/$ver"
out=$EMBCC_ROOT/tests/golden/out/cxx-format-check-$ARCH
rm -rf "$out"; mkdir -p "$out"

n=0
case_() { # name ok|bad call
    src="$out/$1.cc"
    printf '#include <format>\n#include <string>\nstd::string f(int i, double d, const char *s)\n{\n    return %s;\n}\n' "$3" > "$src"
    if "$GXX" -std=c++20 -fsyntax-only "$src" 2> "$out/$1.gxx.err"; then
        gxx=ok
    else
        gxx=bad
    fi
    [ "$gxx" = "$2" ] || { echo "case $1: g++ says $gxx, the case says $2"; exit 1; }
    if err=$("$EMBCC" --target="$TARGET" -I"$INC" -I"$INC/$TARGET" \
                 -I"$NL/include" -c "$src" -o "$out/$1.o" 2>&1); then
        [ "$2" = ok ] || { echo "case $1: compiled, g++ refuses it"; exit 1; }
    else
        [ "$2" = bad ] || { echo "case $1: refused, g++ compiles it:"; echo "$err"; exit 1; }
        echo "$err" | grep -q "call to consteval function 'basic_format_string' is not a constant expression" || {
            echo "case $1: not refused as a format string:"; echo "$err"; exit 1; }
    fi
    n=$((n + 1))
    echo "case $1: $2 for both"
}

case_ plain ok 'std::format("{} {} {}", i, d, s)'
case_ specs ok 'std::format("{:>8.3f}|{:#06x}|{:<5}|{:+d}", d, i, s, i)'
case_ indexed ok 'std::format("{1} {0} {1:*^9}", i, s)'
case_ escaped ok 'std::format("{{}} {}", i)'
case_ nested-width ok 'std::format("{:{}}", s, i)'
case_ int-as-string bad 'std::format("{:s}", i)'
case_ string-as-int bad 'std::format("{:d}", s)'
case_ too-few-args bad 'std::format("{} {}", i)'
case_ unmatched-left bad 'std::format("{", i)'
case_ unmatched-right bad 'std::format("}", i)'
case_ mixed-indexing bad 'std::format("{0} {}", i, d)'
case_ bad-precision bad 'std::format("{:.x}", d)'
echo "$n format strings: EmbCC and g++ agree on which are ill-formed ($ARCH)"
