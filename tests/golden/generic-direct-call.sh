#!/bin/sh
# A _Generic that selects a function is that function's name, so the call
# is direct -- `call @sinf` in the IR, not `call [%v]` through a pointer --
# and can be inlined. That is the shape of every <tgmath.h> call, `_Generic(x, float: sinf, ...)(x)`; sema checked the selection as an
# expression on its own first, where the name decayed to a pointer, and each
# of those calls went through a register. Parenthesized, nested, through
# the header, and on a name that is a static inline function.
set -u
echo "TEST-MARKER generic-direct-call"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/generic-direct-call
rm -rf "${out:?}"; mkdir -p "$out"

cat > "$out/g.c" <<'EOF'
#include <tgmath.h>
float sinf(float);
static inline unsigned twice(unsigned x) { return 2 * x; }
float viatg(float x) { return sqrt(x); }
float plain(float x) { return _Generic(x, float: sinf, default: sin)(x); }
unsigned paren(unsigned x) { return (_Generic(x, unsigned: twice))(x); }
unsigned nested(unsigned x)
{
    return _Generic(x, unsigned: _Generic(1.0, double: twice))(x);
}
EOF

fail=0
for t in x86_64-linux-gnu thumbv7em-none-eabi riscv32-unknown-elf; do
    "$EMBCC" inspect ir --target=$t "$out/g.c" > "$out/$t.ir" 2>&1 || {
        echo "$t: does not compile:"; head -3 "$out/$t.ir"; fail=1; continue; }
    if grep -q 'call \[' "$out/$t.ir"; then
        echo "$t: a call through a pointer:"; grep 'call \[' "$out/$t.ir"
        fail=1
    fi
    for want in sqrtf sinf twice; do
        grep -q "call @$want(" "$out/$t.ir" || {
            echo "$t: no direct call of $want"; fail=1; }
    done
    [ "$(grep -c 'call @twice(' "$out/$t.ir")" = 2 ] || {
        echo "$t: paren and nested do not both call twice directly"; fail=1; }
done
[ $fail = 0 ] || exit 1
echo "generic-direct-call: a _Generic naming a function is a direct call"
