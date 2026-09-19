#!/bin/sh
# A C++ exception unwinding THROUGH functions EmbCC compiled: g++'s code
# catches, EmbCC's C sits in the middle calling back into g++'s code, which
# throws. The unwinder crosses EmbCC's frames with nothing but their
# unwind tables (.eh_frame): where the caller's frame is, where the return
# address is, and — at -O2 — where each callee-saved register the catcher
# keeps its values in was saved. A wrong rule does not fail quietly: the
# catcher's registers come back wrong, or the throw ends in std::terminate
# (which is what happens without the tables; checked too).
set -u
echo "TEST-MARKER unwind-through"
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
out=$EMBCC_ROOT/tests/golden/out/unwind-through-$ARCH
rm -rf "$out"; mkdir -p "$out"

cat > "$out/mid.c" << 'EOF'
/* EmbCC: the frames in the middle, with values live across the call */
typedef long (*cb)(long);
long through(cb f, long a, long b, long c)
{
    long x = a * 3, y = b * 5, z = c * 7;
    long r = f(x + y + z);
    return r + x + y + z;
}
long deeper(cb f, long a)
{
    long keep = a * 19;
    return through(f, a, a + 1, a + 2) + keep;
}
EOF
cat > "$out/top.cc" << 'EOF'
// g++ at -O2: the catcher keeps values in callee-saved registers
#include <stdio.h>
extern "C" long through(long (*)(long), long, long, long);
extern "C" long deeper(long (*)(long), long);
struct E { long v; };
static long thrower(long v) { if (v > 0) throw E{v}; return v; }
__attribute__((noinline)) static long work(long k)
{
    long keep1 = k * 11, keep2 = k * 13, keep3 = k * 17;
    long got = 0;
    try {
        got = k & 1 ? through(thrower, k, k + 1, k + 2) : deeper(thrower, k);
    } catch (E &e) {
        got = e.v;
    }
    return got * 1000 + keep1 + keep2 + keep3;
}
int main()
{
    long a = work(1), b = work(2);
    printf("%ld %ld\n", a, b);
    return a == 34041 && b == 49082 ? 42 : 1;
}
EOF
"$GXX" -std=c++20 -O2 -c "$out/top.cc" -o "$out/top.o" || {
    echo "g++ failed"; exit 1; }
link="$EMBCC_ROOT/tests/harness/$ARCH/link.sh"
for opt in -O0 -O2; do
    "$EMBCC" --target="$TARGET" -funwind-tables $opt -c "$out/mid.c" \
        -o "$out/mid.o" || { echo "embcc failed ($opt)"; exit 1; }
    EMBCC_REF_GXX=$REF "$link" --cxx -o "$out/prog" "$out/top.o" \
        "$out/mid.o" || { echo "link failed ($opt)"; exit 1; }
    t_run "$out/prog"; st=$?
    [ "$st" -eq 42 ] || { echo "through EmbCC's $opt frames: exit $st"; exit 1; }
done
# without the tables the unwinder cannot cross those frames at all
"$EMBCC" --target="$TARGET" -c "$out/mid.c" -o "$out/mid.o" || exit 1
EMBCC_REF_GXX=$REF "$link" --cxx -o "$out/prog" "$out/top.o" "$out/mid.o" ||
    exit 1
t_run "$out/prog" > "$out/none.txt" 2>&1; st=$?
[ "$st" -ne 42 ] || { echo "caught without unwind tables?"; exit 1; }
echo "g++'s exception unwinds through EmbCC's -O0 and -O2 frames ($ARCH):"
echo "caught, with the catcher's callee-saved registers restored"
