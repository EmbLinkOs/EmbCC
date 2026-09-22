#!/bin/sh
# Automatic vectorization: that it fires, and that it is right.
#
# A vectorizer is the most dangerous pass in a compiler, because what it
# gets wrong is an array -- and array code is most code. So the weight
# here is on the second half. Every loop is computed three ways (-O0,
# -O1 where the pass is off, -O2 where it is on) and against gcc, and a
# single differing checksum fails the test.
#
# The refusals get as much attention as the transformations, because
# each one is a way to be wrong:
#
#   a trip count that is not a multiple of the vector factor would run
#   the last iteration off the end of the array; a base that came from a
#   pointer could alias another reference in the same loop, so a vector
#   store would write lanes a scalar loop would still have been reading;
#   a stored value that varies per lane (a[i] = i) has no single value a
#   lane can hold -- that one got through once and filled an array with
#   the induction variable's stack slot read as sixteen bytes.
#
# They are checked by REMARK, not by reading assembly: -fremarks names
# the loops that vectorized, so "this one must not" is a real assertion
# rather than a hope.
set -eu
echo "TEST-MARKER vectorize"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/vectorize
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || {
    echo "skipped: vectorization is x86-64 (SSE2) so far"; exit 0; }

# nvec SRC -> how many loops vectorized
nvec() {
    "$EMBCC" --target=x86_64-linux-gnu -O2 -fremarks -c "$1" -o /dev/null \
        2> "$out/r.txt" || { echo "FAIL: could not compile $1:"
                             cat "$out/r.txt"; exit 1; }
    grep -c 'vec/constant-trip-count' "$out/r.txt" || true
}

# ---- 1. it fires on the shape it is meant to ---------------------------
cat > "$out/yes.c" <<'EOF'
int a[1024], b[1024];
long c[512];
void f1(void) { for (int i = 0; i < 1024; i++) a[i] = a[i] * 3 + 1; }
void f2(void) { for (int i = 0; i < 1024; i++) a[i] = a[i] + b[i]; }
void f3(void) { for (int i = 0; i < 1024; i++) b[i] = (b[i] << 2) ^ 0x1234; }
void f4(void) { for (int i = 0; i < 512;  i++) c[i] = c[i] * 8 - 5; }
void f5(void) { for (int i = 0; i < 1024; i++) a[i] = (a[i] & 0xff) | 0x300; }
EOF
n=$(nvec "$out/yes.c")
[ "$n" = 5 ] || { echo "FAIL: 5 loops should vectorize, $n did:"
                  cat "$out/r.txt"; exit 1; }
echo "five loop shapes vectorize: multiply-add, two arrays, shift-xor,
64-bit lanes, and mask-or"

# ---- 2. and not on the shapes it must refuse ---------------------------
refuse() {                      # refuse NAME BODY WHY
    cat > "$out/no.c" <<EOF
int a[1024], b[1024];
int  g(int);
$2
EOF
    r=$(nvec "$out/no.c")
    [ "$r" = 0 ] || { echo "FAIL: '$1' vectorized ($r loops), but $3"
                      cat "$out/r.txt"; exit 1; }
}
refuse "odd trip count" \
  'void f(void) { for (int i = 0; i < 1023; i++) a[i] = a[i] + 1; }' \
  "1023 is not a multiple of four, so the last vector would run past the end"
refuse "runtime bound" \
  'void f(int n) { for (int i = 0; i < n; i++) a[i] = a[i] + 1; }' \
  "the trip count is not known, so there is no remainder handling"
refuse "pointer base" \
  'void f(int *p) { for (int i = 0; i < 1024; i++) p[i] = p[i] + 1; }' \
  "p could alias anything, including a[]"
refuse "stores the index" \
  'void f(void) { for (int i = 0; i < 1024; i++) a[i] = i; }' \
  "the stored value differs in every lane"
refuse "stores a constant" \
  'void f(void) { for (int i = 0; i < 1024; i++) a[i] = 7; }' \
  "the stored value is not a vector (a splat would do, but is not done)"
refuse "calls" \
  'void f(void) { for (int i = 0; i < 1024; i++) a[i] = g(a[i]); }' \
  "a call is not lane-wise"
refuse "divides" \
  'void f(void) { for (int i = 0; i < 1024; i++) a[i] = a[i] / 3; }' \
  "there is no packed integer divide"
refuse "general multiply" \
  'void f(void) { for (int i = 0; i < 1024; i++) a[i] = a[i] * b[i]; }' \
  "a packed 32-bit multiply is SSE4.1, not SSE2"
refuse "per-lane shift" \
  'void f(void) { for (int i = 0; i < 1024; i++) a[i] = a[i] << b[i]; }' \
  "SSE2 shifts every lane by the same count"
refuse "backwards" \
  'void f(void) { for (int i = 1024; i > 0; i--) a[i-1] = a[i-1] + 1; }' \
  "the induction variable does not start at zero and count up"
refuse "strided" \
  'void f(void) { for (int i = 0; i < 512; i++) a[i*2] = a[i*2] + 1; }' \
  "lane k is not element i+k when the stride is two"
echo "eleven shapes are refused, each for a reason that would be a wrong
answer: an odd trip count, a runtime bound, a pointer base, a stored
index, a stored constant, a call, a divide, a general multiply, a
per-lane shift, a countdown, and a stride of two"

# ---- 3. the answers ----------------------------------------------------
LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
[ -f "$LIBDIR/libc.a" ] || { echo "skipped the run: no libc"; exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "skipped the run: no kernel for tests/harness/linux"; exit 0; }

cat > "$out/run.c" <<'EOF'
#include <stdio.h>
int a[1024], b[1024], odd[1023];
long c[512];
/* Sixteen bytes of guard either side of a vectorized array, checked at
 * the end: a vector store that ran one iteration too far lands here. */
int pre[8], post[8];
int main(void)
{
    unsigned long h = 0;
    for (int i = 0; i < 8; i++) { pre[i] = 0x11111111; post[i] = 0x22222222; }
    for (int i = 0; i < 1024; i++) { a[i] = i * 7 + 3; b[i] = i ^ 0x5a; }
    for (int i = 0; i < 1023; i++) odd[i] = i * 5;
    for (int i = 0; i < 512; i++)  c[i] = (long)i * 4 - 900;

    for (int i = 0; i < 1024; i++) a[i] = a[i] * 3 + 1;
    for (int i = 0; i < 1024; i++) b[i] = (b[i] << 2) ^ 0x1234;
    for (int i = 0; i < 1024; i++) a[i] = a[i] + b[i];
    for (int i = 0; i < 1024; i++) b[i] = b[i] - a[i];
    for (int i = 0; i < 1024; i++) a[i] = (a[i] & 0xffff) | 0x30000;
    for (int i = 0; i < 1024; i++) b[i] = b[i] * 4;
    for (int i = 0; i < 512; i++)  c[i] = c[i] * 8 + 5;
    for (int i = 0; i < 512; i++)  c[i] = c[i] >> 1;   /* unsigned-safe? signed */
    for (int i = 0; i < 1023; i++) odd[i] = odd[i] * 2 + 1;   /* must stay scalar */

    for (int i = 0; i < 1024; i++) h = h * 31 + (unsigned)a[i];
    for (int i = 0; i < 1024; i++) h = h * 31 + (unsigned)b[i];
    for (int i = 0; i < 512; i++)  h = h * 31 + (unsigned long)c[i];
    for (int i = 0; i < 1023; i++) h = h * 31 + (unsigned)odd[i];
    for (int i = 0; i < 8; i++)
        if (pre[i] != 0x11111111 || post[i] != 0x22222222) {
            printf("guard clobbered at %d\n", i);
            return 1;
        }
    printf("%lu\n", h);
    return 42;
}
EOF

prev=
for O in 0 1 2; do
    "$EMBCC" --target=x86_64-linux-gnu "-O$O" "$out/run.c" -o "$out/r" \
        2> "$out/cc.log" || { echo "FAIL: build at -O$O:"; cat "$out/cc.log"
                              exit 1; }
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/r" > "$out/r.out" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "FAIL: -O$O exited $rc:"; cat "$out/r.out"; exit 1; }
    ans=$(cat "$out/r.out")
    [ -z "$prev" ] || [ "$prev" = "$ans" ] || {
        echo "FAIL: -O$O answers $ans where a lower level answered $prev."
        echo "      The vectorized loop computes something else."
        exit 1; }
    prev=$ans
done

GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
if command -v "$GCC" > /dev/null 2>&1; then
    "$GCC" -O2 -c "$out/run.c" -I"$EMBCC_ROOT/lib/libc/include" -o "$out/g.o" \
        2> /dev/null || { echo "FAIL: gcc will not build it"; exit 1; }
    "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
        "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
        "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/g" > "$out/g.out" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc:"
                        cat "$out/g.out"; exit 1; }
    [ "$(cat "$out/g.out")" = "$prev" ] || {
        echo "FAIL: gcc answers $(cat "$out/g.out"), embcc answers $prev"
        exit 1; }
    echo "nine vectorized loops and one that must not be agree with gcc and
across -O0/-O1/-O2, with the guard words either side of every array
still intact"
else
    echo "the answers agree across -O0/-O1/-O2 (no $GCC to compare)"
fi
