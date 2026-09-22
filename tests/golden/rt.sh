#!/bin/sh
# lib/rt — the compiler runtime, against the host's own.
#
# These are the routines the BACKEND calls: operations neither machine
# has an instruction for, which codegen turns into a call. Until they
# existed the Linux target compiled `__int128 a * b` and then failed to
# LINK it, with an undefined `__multi3` and nothing to say why. That was
# D-014's second amendment, and this is the first half of what it
# promised: the integer runtime, testable value by value.
#
# The oracle is the HOST's compiler, which has a native __int128 and its
# own runtime under it. The same source is compiled twice -- once
# natively, once by EmbCC over lib/rt -- and the two must print
# identical text. That is what makes it a check rather than us agreeing
# with ourselves: a rounding rule implemented consistently and wrongly
# passes every self-comparison there is.
#
# What is deliberately NOT compared: conversions of a float too large
# for the integer type. C leaves those undefined, and the host is not an
# oracle for them -- at any optimisation level it does not even call its
# own runtime, it folds them in the compiler, and it answered
# inconsistently between two such values when this was tried. Our
# behaviour there is saturation, and it is checked below as ours.
set -eu
echo "TEST-MARKER rt"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/rt
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || { echo "skipped: the runtime harness here is x86-64"
                          exit 0; }
LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
[ -f "$LIBDIR/librt.a" ] || {
    echo "skipped: no $LIBDIR/librt.a (make libc-linux-x86_64)"; exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "skipped: running it needs a kernel for tests/harness/linux"
    exit 0; }

SRC=$EMBCC_ROOT/tests/golden/rt/rt128.c
HOSTCC=${EMBCC_HOST_CC1:-cc}

# ---- the reference: the host's compiler and the host's runtime -----------
"$HOSTCC" -O1 -o "$out/ref" "$SRC" 2> "$out/ref-cc.log" || {
    echo "skipped: the host compiler will not build the corpus:"
    head -5 "$out/ref-cc.log"; exit 0; }
set +e
"$out/ref" > "$out/ref.txt"
refrc=$?
set -e
[ "$refrc" = 42 ] || {
    echo "FAIL: the reference itself exited $refrc, so there is nothing"
    echo "      to compare against"; exit 1; }

# ---- ours ---------------------------------------------------------------
#
# Through the DRIVER, with no -l of any kind: part of what is being
# checked is that `embcc prog.c -o prog` finds librt beside libc by
# itself, the way it finds crt1.
"$EMBCC" --target=x86_64-linux-gnu -O1 "$SRC" -o "$out/ours" \
    2> "$out/ours-cc.log" || {
    echo "FAIL: embcc could not build the corpus:"
    cat "$out/ours-cc.log"
    echo "      an undefined __multi3 here means the driver did not put"
    echo "      librt.a on the link line"
    exit 1; }
set +e
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/ours" > "$out/ours.txt" 2>&1
ourrc=$?
set -e
[ "$ourrc" = 42 ] || {
    echo "FAIL: our build exited $ourrc:"; tail -10 "$out/ours.txt"; exit 1; }

if ! diff -u "$out/ref.txt" "$out/ours.txt" > "$out/diff.txt"; then
    echo "FAIL: lib/rt disagrees with the host's runtime. First lines:"
    head -20 "$out/diff.txt"
    echo "      ($(grep -c '^[-+]' "$out/diff.txt") differing lines of"
    echo "       $(wc -l < "$out/ref.txt") compared)"
    exit 1
fi
echo "$(wc -l < "$out/ref.txt" | tr -d ' ') lines identical to the host's own
runtime: multiply, divide and remainder signed and unsigned, all three
shifts at every amount, and the conversions both ways"

# ---- complex multiply and divide, against libgcc ------------------------
#
# The oracle here is gcc's own runtime and not the host's, because
# lib/rt implements libgcc's INTERFACE -- __muldc3, __divsc3 and the
# rest -- and libgcc is therefore the implementation whose observable
# behaviour ours has to be defensible against. (clang's compiler-rt uses
# a different algorithm again and differs from libgcc on 605 of the same
# 10683 lines, which is the measurement that settled which reference to
# use.)
#
# The reference is built by the cross gcc and linked against ITS libgcc
# and OUR libc, so the only difference between the two runs is which
# complex routines get called.
GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
CSRC=$EMBCC_ROOT/tests/golden/rt/rtcomplex.c
if ! command -v "$GCC" > /dev/null 2>&1; then
    echo "skipped the complex half: no $GCC to be the reference"
    exit 0
fi
LIBGCC=$("$GCC" -print-libgcc-file-name 2>/dev/null || echo "")
[ -f "$LIBGCC" ] || { echo "skipped the complex half: no libgcc.a"; exit 0; }

"$GCC" -O1 -c "$CSRC" -I"$EMBCC_ROOT/lib/libc/include" -o "$out/cref.o"     2> "$out/cref-cc.log" || {
    echo "skipped the complex half: $GCC will not build the corpus:"
    head -5 "$out/cref-cc.log"; exit 0; }
"$EMBCC_ROOT/embld" -o "$out/cref" "$LIBDIR/crt1.o" "$out/cref.o"     "$LIBDIR/libc.a" "$LIBGCC" 2> "$out/cref-ld.log" || {
    echo "FAIL: linking the gcc reference:"; cat "$out/cref-ld.log"; exit 1; }
set +e
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/cref" > "$out/cref.txt" 2>&1
rc=$?
set -e
[ "$rc" = 42 ] || {
    echo "FAIL: the gcc reference exited $rc"; tail -5 "$out/cref.txt"
    exit 1; }

"$EMBCC" --target=x86_64-linux-gnu -O1 "$CSRC" -o "$out/cours"     2> "$out/cours-cc.log" || {
    echo "FAIL: embcc could not build the complex corpus:"
    cat "$out/cours-cc.log"; exit 1; }
set +e
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/cours" > "$out/cours.txt" 2>&1
rc=$?
set -e
[ "$rc" = 42 ] || {
    echo "FAIL: our build exited $rc"; tail -5 "$out/cours.txt"; exit 1; }

awk -f "$EMBCC_ROOT/tests/golden/rt/compare.awk"     "$out/cref.txt" "$out/cours.txt" > "$out/cmp.txt" || true
res=$(grep '^RESULT ' "$out/cmp.txt")
set -- $res
total=$2; bad=$3; ulp=$4; zs=$5; better=$6
[ "$bad" = 0 ] || {
    echo "FAIL: $bad of $total complex results differ by more than the"
    echo "      rounding a complex divide is allowed. The first few:"
    grep -v '^RESULT ' "$out/cmp.txt" | head -15
    exit 1; }
same=$((total - ulp - zs - better))
echo "complex: $same of $total bit-identical to libgcc -- every multiply,
including all 2401 combinations of zero, infinity and NaN, and every
division Annex G specifies. Of the rest, $ulp differ by one ulp and $zs by
the sign of a zero, both of which C leaves to the implementation; in
$better libgcc returned NaN where Smith's method returns the value
(checked by hand: one is -1e160, to 5e-18)"

# ---- and the saturation, as OUR behaviour -------------------------------
cat > "$out/sat.c" << 'EOF'
/* Out of range is undefined in C, so this is not compared with anyone:
 * it states what lib/rt does, which is saturate at the type's extremes.
 * Saturating is monotone -- a bigger input cannot give a smaller output
 * -- which is the property that makes a wrong answer followable.
 * volatile, so the values reach the runtime rather than the folder. */
#include <stdio.h>
typedef unsigned long long u64;
typedef unsigned __int128 u128;
typedef __int128 s128;
static int eq(u128 v, u64 hi, u64 lo)
{
    return (u64)(v >> 64) == hi && (u64)v == lo;
}
int main(void)
{
    volatile double big = 1e308, neg = -1e308, nan = 0.0;
    nan = nan / nan;
    if (!eq((u128)(s128)big, 0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL))
        return 1;                       /* too large, signed */
    if (!eq((u128)(s128)neg, 0x8000000000000000ULL, 0))
        return 2;                       /* too small, signed */
    if (!eq((u128)big, ~0ULL, ~0ULL))
        return 3;                       /* too large, unsigned */
    if (!eq((u128)(s128)nan, 0, 0))
        return 4;                       /* NaN converts to zero */
    if (!eq((u128)neg, 0, 0))
        return 5;                       /* negative to unsigned is zero */
    printf("saturation: extremes for out of range, zero for NaN\n");
    return 42;
}
EOF
"$EMBCC" --target=x86_64-linux-gnu -O1 "$out/sat.c" -o "$out/sat" \
    2> "$out/sat-cc.log" || {
    echo "FAIL: did not build:"; cat "$out/sat-cc.log"; exit 1; }
set +e
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/sat" > "$out/sat.txt" 2>&1
rc=$?
set -e
[ "$rc" = 42 ] || {
    echo "FAIL: an out-of-range conversion did not saturate (exit $rc)"
    cat "$out/sat.txt"; exit 1; }
head -1 "$out/sat.txt"
