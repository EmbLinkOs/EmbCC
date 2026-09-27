#!/bin/sh
# The AVR target's front end: its data model, and the fact that it
# refuses to emit code rather than emitting another machine's.
#
# AVR is the first target here that is not a flat 32- or 64-bit register
# machine, and three things about it are unlike every other one:
#
#   - `int` is TWO bytes, and so is a POINTER. Every other target has a
#     four-byte int, which is why ty_size() answered 4 outright until AVR
#     made it a target property.
#   - `double` is FOUR bytes -- avr-gcc's documented default. Same
#     reason: it was a constant 8 before.
#   - `char` is SIGNED, matching clang --target=avr and the generated
#     predefined-macro table. avr-gcc is documented as defaulting to
#     -funsigned-char, so this is probably NOT what avr-gcc does -- but
#     there is no avr-gcc here to check, and setting the model from
#     recollection while hand-editing a generated table to agree is how
#     a compiler ends up disagreeing with itself. The assertion below
#     pins what EmbCC actually does; settling it against a real avr-gcc
#     is an open item recorded in the gap document.
#
# The code generator now exists (tests/golden/avr-exec.sh runs its output
# on an ATmega328P), so what this file asserts about it has changed: -c
# must now SUCCEED and produce an ELF32 object for EM_AVR. That is still
# the same guard, pointed the other way -- adding the triple before the
# backend once made the x86-64 code generator the fallback, and
# `--target=avr -c` produced an ELF64 object claiming EM_AVR full of
# x86-64 instructions. It linked. It disassembled as plausible nonsense.
# So the check is now on the header and the machine, which is what would
# catch the same mistake if the dispatch were ever lost again.
set -u
echo "TEST-MARKER avr-target"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-target
rm -rf "$out"; mkdir -p "$out"

# ---- the data model ---------------------------------------------------
cat > "$out/dm.c" <<'CEOF'
_Static_assert(sizeof(char) == 1, "");
_Static_assert(sizeof(short) == 2, "");
_Static_assert(sizeof(int) == 2, "int is 16-bit on AVR");
_Static_assert(sizeof(long) == 4, "");
_Static_assert(sizeof(long long) == 8, "");
_Static_assert(sizeof(void *) == 2, "pointers are 16-bit");
_Static_assert(sizeof(float) == 4, "");
_Static_assert(sizeof(double) == 4, "double is 32-bit on AVR");
_Static_assert(sizeof(long double) == 4, "and so is long double");
/* char is SIGNED here -- clang's AVR answer, and the one the predefined
 * macros agree with. See the note at the top: this may differ from
 * avr-gcc and is deliberately the verifiable choice rather than the
 * remembered one. */
_Static_assert((char)200 < 0, "char is signed, as clang --target=avr has it");
/* The integer promotions follow int's width, which is the part that
 * surprises: a short promotes to a 2-byte int, so `sizeof(s + s)` is 2
 * and not 4. */
_Static_assert(sizeof(1 + 1) == 2, "");
CEOF
"$EMBCC" --target=avr -fsyntax-only "$out/dm.c" || {
    echo "the AVR data model does not hold"; exit 1; }
echo "the data model holds: int 2, pointer 2, double 4, signed char"

# ...and the SAME assertions must fail on a 32-bit-int target, or they
# are not testing anything.
"$EMBCC" --target=thumbv7m-none-eabi -fsyntax-only "$out/dm.c" 2>/dev/null && {
    echo "the AVR assertions also pass on ARMv7-M -- they are vacuous"
    exit 1; }
echo "and does not hold on ARMv7-M, so the target is what decided it"

# ---- the predefined macros AVR code actually tests --------------------
for m in __AVR__ __AVR_ARCH__ __AVR_ATmega328P__ \
         __SIZEOF_INT__ __SIZEOF_POINTER__ __SIZEOF_DOUBLE__ __ELF__; do
    "$EMBCC" --target=avr --dump-predef | grep -q "^#define $m " || {
        echo "missing $m -- AVR headers select on the PART, so they need it"
        exit 1; }
done
printf '#ifndef __AVR__\n#error not avr\n#endif\n#if __SIZEOF_INT__ != 2\n#error int\n#endif\nint ok;\n' \
    > "$out/ifd.c"
"$EMBCC" --target=avr -fsyntax-only "$out/ifd.c" || {
    echo "a normal #ifdef __AVR__ guard does not work"; exit 1; }
echo "the predefined macros are there and an #ifdef __AVR__ guard works"

# ---- and the object it emits is an AVR object ------------------------
printf 'int f(int a){ return a + 1; }\n' > "$out/f.c"
"$EMBCC" --target=avr -c "$out/f.c" -o "$out/f.o" 2> "$out/e.txt" || {
    echo "--target=avr -c failed:"; head -4 "$out/e.txt"; exit 1; }
# Byte 4 of an ELF header is EI_CLASS: 1 is ELFCLASS32. Bytes 18-19 are
# e_machine, little-endian: 83 (0x53) is EM_AVR. Read out of the file
# rather than asked of a tool, so this test needs none.
cls=$(od -An -tu1 -j4 -N1 "$out/f.o" | tr -d ' ')
mach=$(od -An -tu1 -j18 -N1 "$out/f.o" | tr -d ' ')
[ "$cls" = 1 ] || {
    echo "the object is ELF class $cls, not 1 (ELFCLASS32) -- an AVR"
    echo "pointer is two bytes and its objects are 32-bit"; exit 1; }
[ "$mach" = 83 ] || {
    echo "the object claims machine $mach, not 83 (EM_AVR) -- so whatever"
    echo "is in it came from another machine's backend"; exit 1; }
echo "-c produces an ELFCLASS32 object for EM_AVR, so no object can carry"
echo "another machine's instructions under an EM_AVR header"

# The front end still works, which is the point of having the target at
# all before the backend exists.
"$EMBCC" --target=avr -fsyntax-only "$out/f.c" || {
    echo "-fsyntax-only does not work for AVR"; exit 1; }
"$EMBCC" --target=avr -E "$out/f.c" > /dev/null || {
    echo "-E does not work for AVR"; exit 1; }
echo "-E and -fsyntax-only work as well, so a program can be checked for"
echo "this target without being built for it"
