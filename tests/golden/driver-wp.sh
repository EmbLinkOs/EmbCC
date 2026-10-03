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
