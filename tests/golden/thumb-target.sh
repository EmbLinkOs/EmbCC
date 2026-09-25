#!/bin/sh
# The ARMv7-M target's DATA MODEL (D-015), and the refusal behind it.
#
# This target has no code generator yet, so what can be tested is exactly
# what exists: that the triple is accepted, that the front end believes the
# right things about the machine, and that asking for an object says so
# rather than producing one. The third is the point — a wrong answer here
# would be an object full of x86-64 instructions under an EM_ARM header.
set -u
echo "TEST-MARKER thumb-target"
. "$(dirname "$0")/../lib.sh"

T=thumbv7m-none-eabi
tmp=${TMPDIR:-/tmp}/thumb-target.$$
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT

# 1. Every spelling of the target resolves, and the canonical name comes back.
for alias in thumbv7m-none-eabi thumbv7m thumbv7em-none-eabi thumbv7em \
             armv7m-none-eabi arm-none-eabi; do
    got=$("$EMBCC" --target="$alias" -dumpmachine) || {
        echo "--target=$alias was not accepted"; exit 1; }
    [ "$got" = "$T" ] || {
        echo "--target=$alias -dumpmachine said '$got', not '$T'"; exit 1; }
done
echo "six spellings of ARMv7-M resolve to $T"

# 2. The data model, asked of the compiler rather than asserted about it.
#    _Static_assert over TYPE NAMES is what this front end folds today;
#    the literal-typing rules go with the backend that can run them.
cat > "$tmp/model.c" <<'EOF'
_Static_assert(sizeof(void *) == 4, "ILP32: pointers are four bytes");
_Static_assert(sizeof(long) == 4, "ILP32: long is four bytes");
_Static_assert(sizeof(long long) == 8, "long long stays eight");
_Static_assert(sizeof(unsigned long long) == 8, "and so does its unsigned");
_Static_assert(sizeof(int) == 4 && sizeof(short) == 2, "int and short");
_Static_assert(sizeof(double) == 8, "double");
_Static_assert(sizeof(long double) == 8, "AAPCS32: long double IS a double");
EOF
"$EMBCC" --target="$T" -fsyntax-only "$tmp/model.c" || {
    echo "the ARMv7-M data model is not what AAPCS32 says"; exit 1; }
echo "ILP32 data model holds (ptr 4, long 4, long long 8, long double 8)"

# The same file must FAIL on a 64-bit target — otherwise the assertions
# above are passing for some reason other than the target being read.
if "$EMBCC" --target=x86_64-elf -fsyntax-only "$tmp/model.c" 2>/dev/null; then
    echo "the ILP32 assertions also hold on x86-64 — nothing was tested"
    exit 1
fi
echo "and does not hold on x86-64, so the target is what decided it"

# 3. char and wchar_t are unsigned here, as on aarch64 and unlike x86-64.
"$EMBCC" --target="$T" --dump-predef | grep -q '^#define __CHAR_UNSIGNED__ 1' || {
    echo "__CHAR_UNSIGNED__ is missing from the ARMv7-M table"; exit 1; }
"$EMBCC" --target="$T" --dump-predef | grep -q '^#define __SIZEOF_POINTER__ 4' || {
    echo "__SIZEOF_POINTER__ disagrees with sizeof(void *)"; exit 1; }
echo "the predefined macros agree with the front end's own sizes"

# 4. __int128 is refused by name, not lowered into something no backend has.
printf '__int128 x;\n' > "$tmp/i128.c"
if "$EMBCC" --target="$T" -fsyntax-only "$tmp/i128.c" 2>"$tmp/i128.err"; then
    echo "__int128 was accepted on a 32-bit target"; exit 1
fi
grep -q "__int128 does not exist on this target" "$tmp/i128.err" || {
    echo "__int128 failed for the wrong reason:"; cat "$tmp/i128.err"; exit 1; }
echo "__int128 is refused by name"

# 5. THE RULE: no backend means no object, said out loud.
printf 'int add(int a, int b) { return a + b; }\n' > "$tmp/add.c"
if "$EMBCC" --target="$T" -c "$tmp/add.c" -o "$tmp/add.o" 2>"$tmp/add.err"; then
    echo "an object was produced for a target with no code generator"; exit 1
fi
grep -q "has no code generator yet" "$tmp/add.err" || {
    echo "-c failed for the wrong reason:"; cat "$tmp/add.err"; exit 1; }
[ ! -s "$tmp/add.o" ] || { echo "a refused compile still wrote an object"; exit 1; }
echo "-c refuses loudly and writes nothing"

# 6. What DOES work today works: preprocessing and checking.
"$EMBCC" --target="$T" -E "$tmp/add.c" > /dev/null || {
    echo "-E does not work for this target"; exit 1; }
"$EMBCC" --target="$T" -fsyntax-only "$tmp/add.c" || {
    echo "-fsyntax-only does not work for this target"; exit 1; }
echo "-E and -fsyntax-only work for ARMv7-M"
