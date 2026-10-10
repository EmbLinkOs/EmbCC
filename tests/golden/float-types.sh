#!/bin/sh
# The extended floating-point type spellings, and what each target has.
#
# The point of this test is that three different answers are all correct
# and the compiler has to give the RIGHT one per target:
#
#   _Float32 / _Float64   ARE float and double everywhere here, because
#                         both are IEEE binary32/binary64. Not "close
#                         enough" -- the same type, so _Generic matches.
#   _Float128             is `long double` where long double IS IEEE
#                         binary128 (aarch64, RISC-V). On x86-64 long
#                         double is x87's 80-bit extended -- a different
#                         format with a different exponent range and an
#                         explicit integer bit -- so it is NOT that type
#                         and is refused by name. On ARMv7-M long double
#                         is plain double, so there is no such type.
#   _Float16 / __fp16     does not exist here at any width. It is an
#                         incomplete type: a declaration may name it
#                         (macOS's <math.h> declares __fabsf16 and its
#                         kin), and an object, a call, a sizeof or a cast
#                         is refused rather than widened to float, which
#                         would give 24 bits of mantissa where the
#                         program asked for 11 (THE RULE).
set -u
echo "TEST-MARKER float-types"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/float-types
rm -rf "$out"; mkdir -p "$out"

# ---- _Float32 / _Float64: the same type, on every target --------------
cat > "$out/i.c" <<'CEOF'
_Static_assert(sizeof(_Float32) == 4, "");
_Static_assert(sizeof(_Float64) == 8, "");
/* _Generic is the test that matters: "same size" would pass for a
 * distinct type too, and these must be the SAME type. */
_Static_assert(_Generic((_Float32)0, float:  1, default: 0), "");
_Static_assert(_Generic((_Float64)0, double: 1, default: 0), "");
_Float32 f(_Float32 a, _Float32 b) { return a * b + 1.0f; }
_Float64 d(_Float64 a)             { return a / 3.0; }
float  takes_float(_Float32 v)  { return v; }
double takes_double(_Float64 v) { return v; }
CEOF
for t in x86_64-linux-gnu aarch64-linux-gnu riscv64-unknown-elf \
         riscv32-unknown-elf thumbv7m-none-eabi; do
    "$EMBCC" --target=$t -c "$out/i.c" -o "$out/i.o" 2> "$out/i.err" || {
        echo "$t: the interchange types do not compile:"
        head -3 "$out/i.err"; exit 1; }
done
echo "_Float32 and _Float64 are float and double on all five targets"

# ---- _Float128: present where the target has binary128 ----------------
cat > "$out/q.c" <<'CEOF'
_Static_assert(sizeof(_Float128) == 16, "");
_Static_assert(_Generic((__float128)0, long double: 1, default: 0), "");
CEOF
"$EMBCC" --target=aarch64-linux-gnu -fsyntax-only "$out/q.c" || {
    echo "aarch64: _Float128 is not long double, and should be"; exit 1; }
echo "aarch64: _Float128 is long double, which is IEEE binary128 there"

# And ABSENT, by name, where it is not that format.
for t in x86_64-linux-gnu thumbv7m-none-eabi; do
    if "$EMBCC" --target=$t -fsyntax-only "$out/q.c" 2> "$out/q.err"; then
        echo "$t: _Float128 was accepted, and that target has no binary128"
        exit 1
    fi
    grep -q '_Float128 is not supported' "$out/q.err" || {
        echo "$t: the refusal does not name _Float128:"
        head -2 "$out/q.err"; exit 1; }
done
echo "x86-64 and ARMv7-M refuse _Float128 by name, each for its own reason"

# The arithmetic, run: binary128 must hold a value a double cannot.
cat > "$out/qr.c" <<'CEOF'
#include <stdio.h>
__float128 add(__float128 a, __float128 b) { return a + b; }
__float128 mul(__float128 a, __float128 b) { return a * b; }
int main(void)
{
    __float128 a = 1, b = 3;
    printf("%g %g\n", (double)add(a, b), (double)mul(add(a, b), b));
    /* 2^-80 next to 1: representable with 113 bits of mantissa, not
     * with 53. This is what makes it binary128 and not a renamed
     * double, and it is the assertion a size check cannot make. */
    __float128 one = 1, eps = 1;
    double done = 1, deps = 1;
    for (int i = 0; i < 80; i++) { eps /= 2; deps /= 2; }
    printf("%d %d\n", (int)((one + eps) != one), (int)((done + deps) != done));
    return 0;
}
CEOF
# t_link/t_run follow $ARCH, which the aarch64 matrix run sets. Running
# this half from an x86-64 run would link aarch64 objects with the x86
# linker, so it belongs to that run and is skipped here.
if [ "$ARCH" = aarch64 ]; then
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target="$TARGET" $opt -c "$out/qr.c" -o "$out/qr$opt.o" ||
            { echo "aarch64 $opt: does not compile"; exit 1; }
        t_link "$out/qr$opt" "$out/qr$opt.o" ||
            { echo "aarch64 $opt: could not link"; exit 1; }
        t_run "$out/qr$opt" > "$out/qr$opt.txt" 2>&1
        got=$(tr '\n' ' ' < "$out/qr$opt.txt")
        [ "$got" = "4 12 1 0 " ] || {
            echo "aarch64 $opt: binary128 arithmetic gave '$got', wanted '4 12 1 0 '"
            exit 1; }
    done
    echo "aarch64: binary128 adds, multiplies, and holds 2^-80 next to 1"
else
    echo "SKIP the binary128 run: it belongs to the aarch64 matrix run"
fi

# ---- _Float16: declared, never a value, at every target ----------------
printf '_Float16 h;\n'        > "$out/h1.c"
printf '__fp16 h;\n'          > "$out/h2.c"
printf 'extern _Float16 g(_Float16);\nextern _Float16 *p;\nint f(void) { return (int)sizeof(_Float16); }\n' > "$out/h3.c"
printf 'extern _Float16 g(_Float16);\nextern _Float16 *p;\nvoid f(void) { g(*p); }\n' > "$out/h4.c"
printf 'int f(float x) { return (int)(_Float16)x; }\n' > "$out/h5.c"
for f in h1 h2 h3 h4 h5; do
    if "$EMBCC" -fsyntax-only "$out/$f.c" 2> "$out/$f.err"; then
        echo "$f: a value of a 16-bit float type was accepted, and none exists"; exit 1
    fi
    grep -q 'incomplete.* _Float16\|cannot cast to _Float16' "$out/$f.err" || {
        echo "$f: the refusal does not name _Float16:"
        head -2 "$out/$f.err"; exit 1; }
done
printf 'extern _Float16 g(_Float16);\nextern __fp16 *p;\nextern _Float16 v;\nint f(void) { return p != 0; }\n' > "$out/h6.c"
"$EMBCC" -fsyntax-only "$out/h6.c" 2> "$out/h6.err" || {
    cat "$out/h6.err"; echo "h6: a declaration naming _Float16 was refused"; exit 1; }
# the same rule for any incomplete type: a call returning or passing one
# copies a value nobody has the size of. It compiled, and moved nothing.
printf 'struct S;\nextern struct S s;\nextern struct S k(struct S);\nvoid f(void) { k(s); }\n' > "$out/inc.c"
if "$EMBCC" -fsyntax-only "$out/inc.c" 2> "$out/inc.err"; then
    echo "a call with an incomplete return type was accepted"; exit 1
fi
grep -q "calling 'k' with incomplete return type struct S" "$out/inc.err" || {
    cat "$out/inc.err"; echo "the incomplete call was refused for another reason"; exit 1; }
printf 'struct S;\nextern struct S s;\nvoid k(struct S);\nvoid f(void) { k(s); }\n' > "$out/inc2.c"
if "$EMBCC" -fsyntax-only "$out/inc2.c" 2> "$out/inc2.err"; then
    echo "an argument of incomplete type was accepted"; exit 1
fi
grep -q "argument 1 has incomplete type struct S" "$out/inc2.err" || {
    cat "$out/inc2.err"; echo "the incomplete argument was refused for another reason"; exit 1; }
echo "_Float16 and __fp16 may be declared; an object, a sizeof, a cast or a call is refused by name, as is a call with any incomplete type"

# ---- _Generic as a constant expression --------------------------------
# It folds in the PARSER now, which is where a static assertion and an
# array bound are evaluated -- sema resolving _Generic later is too late
# for either. Each line below failed before.
cat > "$out/g.c" <<'CEOF'
_Static_assert(_Generic(1,    int:    1, default: 0), "int");
_Static_assert(_Generic(1L,   int: 0, long: 1, default: 0), "long");
_Static_assert(_Generic(1.0f, float:  1, default: 0), "float");
_Static_assert(_Generic("s",  char *: 1, default: 0), "an array decays");
_Static_assert(_Generic(1.0,  float: 0, default: 7) == 7, "the default arm");
int arr[_Generic(0, int: 4, default: 1)];
_Static_assert(sizeof arr == 16, "an array bound, not just an assertion");
/* The controlling expression is NOT evaluated (C11 6.5.1.1), so an arm
 * that is not selected need not be a constant expression. */
_Static_assert(_Generic(1, int: 1, default: (1/0)), "the other arm is not folded");
CEOF
"$EMBCC" -fsyntax-only "$out/g.c" || {
    echo "a _Generic did not fold as a constant expression"; exit 1; }
echo "_Generic folds as a constant expression: assertions and array bounds"

# sizeof a string literal folds too, and gives the ARRAY's size.
cat > "$out/s.c" <<'CEOF'
_Static_assert(sizeof("abc") == 4, "");
_Static_assert(sizeof("") == 1, "");
char c[sizeof "hello"];
_Static_assert(sizeof c == 6, "");
CEOF
"$EMBCC" -fsyntax-only "$out/s.c" || {
    echo "sizeof a string literal does not fold"; exit 1; }
echo "sizeof a string literal folds to its array's size"
