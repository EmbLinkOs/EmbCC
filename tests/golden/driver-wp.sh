#!/bin/sh
# -Wp,OPTIONS reaches the preprocessor.
#
# It was taken for a warning name -- "is not a warning EmbCC has" -- and
# the -D it carried was lost, so the `#ifdef FEATURE` it was meant to
# switch on was quietly off.
set -u
echo "TEST-MARKER driver-wp"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/driver-wp
case "$EMBCC" in /*) EMBCC_ABS=$EMBCC ;; *) EMBCC_ABS=$PWD/$EMBCC ;; esac
rm -rf "${out:?}"; mkdir -p "$out/inc"
printf '#define FROM_INC 5\n' > "$out/inc/h.h"
cat > "$out/wp.c" <<'SRC'
#include "h.h"
#ifndef X
#error -Wp,-DX was lost
#endif
#ifdef Y
#error -Wp,-UY was lost
#endif
int x = X + FROM_INC;
SRC
"$EMBCC" -DY -Wp,-DX=1,-UY,-I"$out/inc" -c "$out/wp.c" -o "$out/wp.o" 2> "$out/err" || {
    echo "FAIL: -Wp,-D,-U,-I:"; cat "$out/err"; exit 1; }
if "$EMBCC" -Wp,-MD,"$out/d" -c "$out/wp.c" -o "$out/wp.o" 2> "$out/err2"; then
    echo "FAIL: -Wp,-MD was accepted and ignored"; exit 1
fi
grep -q "preprocessor option '-MD'" "$out/err2" || {
    echo "FAIL: the refusal does not name the option:"; cat "$out/err2"; exit 1; }
echo "driver-wp: -Wp,-D,-U,-I are applied in order; anything else is refused by name"

# -include FILE: listed in --help, and "unknown argument". It is read as
# if the main file began with #include "FILE", from the working
# directory first and then the include path, and the main file's lines
# keep their numbers.
mkdir -p "$out/sub"
printf '#define FROM_CWD 3\n' > "$out/pre.h"
printf 'int v = FROM_CWD + FROM_INC;\nint line = __LINE__;\n' > "$out/sub/m.c"
( cd "$out" && "$EMBCC_ABS" -include pre.h -include h.h -Iinc -E sub/m.c ) \
    > "$out/inc.i" 2> "$out/inc.err" || {
    echo "FAIL: -include:"; cat "$out/inc.err"; exit 1; }
grep -q 'int v = 3 + 5;' "$out/inc.i" && grep -q 'int line = 2;' "$out/inc.i" || {
    echo "FAIL: -include gave:"; cat "$out/inc.i"; exit 1; }
if "$EMBCC" -include "$out/nosuch.h" -c "$out/wp.c" -o "$out/x.o" 2> "$out/ni.err"; then
    echo "FAIL: a missing -include file was ignored"; exit 1
fi
grep -q 'cannot find -include file' "$out/ni.err" || {
    echo "FAIL: the missing -include file is not named:"; cat "$out/ni.err"; exit 1; }
echo "driver-wp: -include reads its file first, from the working directory or the include path"
