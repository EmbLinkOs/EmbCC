#!/bin/sh
# noexcept: an exception leaving a noexcept function calls std::terminate
# (C++ [except.terminate]) even with a handler waiting above it — EmbCC
# gives such a function a catch-all region whose pad calls
# __cxa_call_terminate. The program must end in libstdc++'s terminate
# message, never in the handler.
set -u
echo "TEST-MARKER cxx-noexcept"
. "$(dirname "$0")/../lib.sh"

if [ "$ARCH" = aarch64 ]; then
    REF=$AARCH64_REF_GXX NL=$AARCH64_NEWLIB
else
    REF=$X86_REF_GXX NL=$X86_NEWLIB
fi
if [ ! -f "$REF/$TARGET/lib/libstdc++.a" ]; then
    echo "skipped: no reference libstdc++ at $REF"
    exit 0
fi
out=$EMBCC_ROOT/tests/golden/out/cxx-noexcept-$ARCH
rm -rf "$out"; mkdir -p "$out"
cat > "$out/ne.cc" << 'EOF'
#include <stdio.h>
static void thrower() { throw 5; }
static void safe() noexcept { thrower(); }
int main()
{
    try {
        safe();
    } catch (int) {
        printf("caught: noexcept ignored\n");
        return 42;
    }
    return 43;
}
EOF
"$EMBCC" --target="$TARGET" -I"$NL/include" -c "$out/ne.cc" -o "$out/ne.o" ||
    { echo "embcc failed"; exit 1; }
EMBCC_REF_GXX=$REF "$EMBCC_ROOT/tests/harness/$ARCH/link.sh" --cxx \
    -o "$out/ne" "$out/ne.o" || { echo "link failed"; exit 1; }
res=$(t_run "$out/ne" 2>&1)
case "$res" in
*"terminate called after throwing an instance of 'int'"*) ;;
*) echo "expected std::terminate, got: $res"; exit 1 ;;
esac
echo "an exception leaving a noexcept function terminates ($ARCH)"
