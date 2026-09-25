#!/bin/sh
# Algebra on the IR: the identities, and moving a constant.
#
# `(x + 1) + 1` is two adds, and the second waits for the first. `x + 2`
# is one add that waits for nothing. Both are the same value, and the
# second is the one to emit -- but "the same value" is a claim about
# two's-complement arithmetic at a fixed width, and it stops being true
# the moment a width or a signedness is got wrong. So each rewrite here
# is checked twice: once on the IR, that it happened, and once by
# RUNNING the program against gcc at every optimization level, over
# inputs that are negative, that are the extremes of their type, and
# that wrap. The wrapping is done in UNSIGNED: signed overflow is
# undefined, so a case built on it would not be comparing two compilers,
# it would be comparing two guesses.
set -eu
echo "TEST-MARKER algebra"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/algebra
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || { echo "skipped: the runner here is x86-64"; exit 0; }

ir() {                          # ir SRC -> $out/ir.txt
    "$EMBCC" inspect ir --target=x86_64-linux-gnu -O2 "$1" > "$out/ir.txt" \
        2> "$out/err.txt" || { echo "FAIL: could not dump IR for $1:"
                               cat "$out/err.txt"; exit 1; }
}

# ---- 1. a constant moves through a chain -------------------------------
cat > "$out/chain.c" <<'EOF'
long f(long x) { long a = x + 1; long b = a + 2; long c = b + 4; return c; }
EOF
ir "$out/chain.c"
nadd=$(grep -cE '^  %[0-9]+ = add' "$out/ir.txt" || true)
[ "$nadd" = 1 ] || {
    echo "FAIL: $nadd adds where one would do -- the constants did not"
    echo "      come together:"; cat "$out/ir.txt"; exit 1; }
grep -qE 'add\.8s? %[0-9]+, #7' "$out/ir.txt" || {
    echo "FAIL: the one add is not of 7:"; cat "$out/ir.txt"; exit 1; }
echo "three adds of a constant become one add of their sum"

# ---- 2. the identities -------------------------------------------------
cat > "$out/ident.c" <<'EOF'
int same_sub(int x) { return x - x; }
int same_xor(int x) { return x ^ x; }
int same_and(int x) { return x & x; }
int same_or (int x) { return x | x; }
int same_eq (int x) { return x == x; }
int same_lt (int x) { return x < x; }
EOF
ir "$out/ident.c"
for op in sub xor and or cmp; do
    grep -qE "^  %[0-9]+ = $op" "$out/ir.txt" && {
        echo "FAIL: an '$op' of a value with itself survives:"
        cat "$out/ir.txt"; exit 1; }
done
echo "x-x, x^x, x&x, x|x, x==x and x<x all resolve without an operation"

# ---- 3. the constant is on the right -----------------------------------
#
# Value numbering keys an operation on its operands IN ORDER, so `1 + x`
# and `x + 1` were two different values of the same expression and
# neither ever matched the other. One canonical order and they do.
cat > "$out/canon.c" <<'EOF'
int g;
int f(int x) { int a = 1 + x; int b = x + 1; g = a; return a + b; }
EOF
ir "$out/canon.c"
nadd=$(grep -cE '^  %[0-9]+ = add\.[0-9]+s? %[0-9]+, #1' "$out/ir.txt" || true)
[ "$nadd" -le 1 ] || {
    echo "FAIL: '1 + x' and 'x + 1' are still two computations:"
    cat "$out/ir.txt"; exit 1; }
echo "'1 + x' and 'x + 1' are one value"

# ---- 3b. a constant operand does not hide a redundant expression -------
#
# Global CSE keys an expression on its operands, and a LITERAL is not
# numbered by that pass -- rematerialising one costs less than keeping
# it live -- so two blocks that each need `-2` hold it in two different
# temps. Keyed by temp, `x & -2` in one block and `x & -2` in another
# were two different values and never matched. Keyed by VALUE they do,
# which is very nearly all of what that pass had been missing.
#
# Built with -fno-pre, because partial redundancy elimination keys
# constants the same way and runs first in the round, so with it on this
# redundancy is gone before global CSE is asked again and the count
# below reads zero. The emitted code is identical either way -- what
# moves is which pass gets the credit -- and the claim under test here
# is about global CSE's keying, so the other pass is held out of it.
cat > "$out/gcse.c" <<'EOF'
void sink(long);
void f(long a, long w)
{
    if ((a & -2) < w) return;
    sink((a & -2) - w);
}
EOF
n=$("$EMBCC" --target=x86_64-linux-gnu -O2 -fno-pre -fremarks -c "$out/gcse.c" \
      -o /dev/null 2>&1 | sed -n 's/.*, \([0-9]*\) global cse.*/\1/p' |
    awk '{s+=$1} END {print s+0}')
[ "$n" -ge 1 ] || {
    echo "FAIL: '(a & -2)' computed in two blocks was not recognised as one"
    echo "      value -- the constant is keyed by the temp holding it, not"
    echo "      by what it is."
    exit 1; }
echo "an expression with a literal operand is one value across blocks"

# ---- 4. and all of it still computes the same thing --------------------
#
# The IR checks above say a rewrite fired. This says it was right, at
# the places where two's-complement arithmetic is the whole question:
# wrapping, the extremes of each width, and a right shift of a negative.
cat > "$out/run.c" <<'EOF'
#include <stdio.h>
#include <limits.h>

/* Signed chains, fed only values where nothing overflows -- signed
 * overflow is undefined, so a program that relies on it is not
 * comparing two compilers, it is comparing two guesses. Wrapping is
 * tested below, in unsigned, where it is defined. */
static int    chain_i(int x)   { int a = x + 1; int b = a + 2; return b + 4; }
static int    unchain_i(int x) { int a = x - 3; int b = a + 1; return b - 5; }
static long   chain_l(long x)  { long a = x + 1; long b = a + 2; return b + 4; }
static int    mulchain(int x)  { int a = x * 3; int b = a * 5; return b * 7; }
static int    sarchain(int x)  { int a = x >> 3; return a >> 5; }
static int    ident(int x)
{ return (x - x) + (x ^ x) + (x & x) + (x | x) + (x == x) + (x < x); }

/* Unsigned, where every one of these wraps and must wrap the same way. */
static unsigned uchain(unsigned x)   { unsigned a = x + 1; unsigned b = a + 2;
                                       return b + 4; }
static unsigned umul(unsigned x)     { unsigned a = x * 3; unsigned b = a * 5;
                                       return b * 7; }
static unsigned andchain(unsigned x) { unsigned a = x & 0xff0fu; return a & 0x0fffu; }
static unsigned orchain(unsigned x)  { unsigned a = x | 0x00f0u; return a | 0x0f00u; }
static unsigned xorchain(unsigned x) { unsigned a = x ^ 0x1234u; return a ^ 0x5678u; }
static unsigned shlchain(unsigned x) { unsigned a = x << 3; return a << 5; }
static unsigned shrchain(unsigned x) { unsigned a = x >> 3; return a >> 5; }
/* the two shifts sum to exactly the width: NOT one shift of 32 */
static unsigned shlfull(unsigned x)  { unsigned a = x << 16; return a << 16; }
static unsigned wrap(unsigned x)     { unsigned a = x + 0x7fffffffu; return a + 1; }
static unsigned unwrap(unsigned x)   { unsigned a = x + 0x7fffffffu;
                                       return a - 0x7fffffffu; }

int main(void)
{
    /* nothing here overflows the signed chains above */
    static const int safe[] = { 0, 1, -1, 2, -2, 7, -7, 1000, -1000,
                                1 << 20, -(1 << 20) };
    /* and everything is fair game for the unsigned ones */
    static const unsigned ext[] = { 0u, 1u, 2u, 0x7fffffffu, 0x80000000u,
                                    0xffffffffu, 0xfffffffeu, 0x12345678u };
    unsigned long h = 0;
    for (unsigned k = 0; k < sizeof safe / sizeof safe[0]; k++) {
        int v = safe[k];
        h = h*1000003u + (unsigned)chain_i(v);
        h = h*1000003u + (unsigned)unchain_i(v);
        h = h*1000003u + (unsigned long)chain_l(v);
        h = h*1000003u + (unsigned)mulchain(v);
        h = h*1000003u + (unsigned)sarchain(v);
        h = h*1000003u + (unsigned)ident(v);
    }
    for (unsigned k = 0; k < sizeof ext / sizeof ext[0]; k++) {
        unsigned u = ext[k];
        h = h*1000003u + uchain(u);
        h = h*1000003u + umul(u);
        h = h*1000003u + andchain(u);
        h = h*1000003u + orchain(u);
        h = h*1000003u + xorchain(u);
        h = h*1000003u + shlchain(u);
        h = h*1000003u + shrchain(u);
        h = h*1000003u + shlfull(u);
        h = h*1000003u + wrap(u);
        h = h*1000003u + unwrap(u);
        h = h*1000003u + (unsigned)sarchain((int)u);
    }
    printf("%lu\n", h);
    return 42;
}
EOF

LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
if [ -f "$LIBDIR/libc.a" ] &&
   "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1
then
    prev=
    for O in 0 1 2; do
        "$EMBCC" --target=x86_64-linux-gnu "-O$O" "$out/run.c" \
            -o "$out/r" 2> "$out/cc.log" || {
            echo "FAIL: build at -O$O:"; cat "$out/cc.log"; exit 1; }
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/r" \
            > "$out/r.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: exited $rc at -O$O:"
                            cat "$out/r.txt"; exit 1; }
        ans=$(cat "$out/r.txt")
        [ -z "$prev" ] || [ "$prev" = "$ans" ] || {
            echo "FAIL: answers $ans at -O$O where a lower level said $prev."
            echo "      A rewrite changed the value at some width."
            exit 1; }
        prev=$ans
    done
    echo "the same answer at -O0, -O1 and -O2 ($prev)"

    GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
    if command -v "$GCC" > /dev/null 2>&1; then
        "$GCC" -O2 -c "$out/run.c" -I"$EMBCC_ROOT/lib/libc/include" \
            -o "$out/g.o" 2> /dev/null || {
            echo "FAIL: gcc will not build the case"; exit 1; }
        "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
            "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
            "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/g" \
            > "$out/g.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc"; exit 1; }
        [ "$(cat "$out/g.txt")" = "$prev" ] || {
            echo "FAIL: gcc answers $(cat "$out/g.txt"), embcc answers $prev"
            exit 1; }
        echo "and the same answer as gcc, wrapping and extremes included"
    fi
else
    echo "(not run: needs a kernel and a libc for tests/harness/linux)"
fi
