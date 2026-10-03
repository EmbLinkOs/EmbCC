#!/bin/sh
# ARM build attributes, and the linker check they exist for.
#
# .ARM.attributes is how an object says what it was built FOR, and the
# only place downstream that can refuse a combination which cannot work.
# Tag_ABI_VFP_args is the one that matters: 0 means floating-point
# arguments travel in the CORE registers (the base standard, what
# -mfloat-abi=soft means) and 1 means they travel in s0-s15.
#
# EmbCC emitted no attributes at all, which sounds harmless and is the
# opposite: with nothing to compare, a soft-float object links against a
# hard-float one without complaint and the callee reads its arguments
# from registers the caller never wrote. That is a miscompilation
# produced at LINK time, past every check the compiler makes.
#
# GNU ld does this comparison, and EmbCC does not depend on GNU ld -- so
# EmbCC's own linker does it. That is what the second half tests, and it
# tests it against objects built by another toolchain, because
# interoperating with one is the entire point of the tags.
set -u
echo "TEST-MARKER arm-abi-tags"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/arm-abi-tags
rm -rf "$out"; mkdir -p "$out"
RE=${EMBCC_LLVM_READELF:-llvm-readelf}

printf 'int f(int a){ return a + 1; }\n' > "$out/t.c"

# ---- the section is there, and says the right things -------------------
for t in thumbv7m-none-eabi thumbv7em-none-eabi; do
    "$EMBCC" --target=$t -c "$out/t.c" -o "$out/$t.o" || {
        echo "$t: does not compile"; exit 1; }
    "$RE" -S "$out/$t.o" | grep -q '\.ARM\.attributes' || {
        echo "$t: the object carries no .ARM.attributes"; exit 1; }
done
echo "both ARMv7-M and ARMv7E-M objects carry .ARM.attributes"

if command -v "$RE" > /dev/null 2>&1 &&
   "$RE" --arch-specific "$out/thumbv7m-none-eabi.o" > "$out/a.txt" 2>&1 &&
   grep -q FileAttributes "$out/a.txt"; then
    # The encoding is only useful if a reader that did not write it can
    # decode it -- a wrong length field produces a section every tool
    # rejects, and both lengths in this format count themselves.
    grep -q 'TagName: CPU_arch' "$out/a.txt" || {
        echo "the attributes do not decode:"; head -12 "$out/a.txt"; exit 1; }
    grep -A3 'TagName: CPU_arch$' "$out/a.txt" | grep -q 'ARM v7' || {
        echo "thumbv7m does not report ARM v7:"; cat "$out/a.txt"; exit 1; }
    "$RE" --arch-specific "$out/thumbv7em-none-eabi.o" > "$out/b.txt" 2>&1
    grep -A3 'TagName: CPU_arch$' "$out/b.txt" | grep -q 'ARM v7E-M' || {
        echo "thumbv7em does not report ARM v7E-M -- the name is still an
alias for the base profile:"; cat "$out/b.txt"; exit 1; }
    # The float ABI, stated positively rather than by absence.
    grep -q 'TagName: ABI_VFP_args' "$out/a.txt" || {
        echo "Tag_ABI_VFP_args is missing, so nothing downstream can tell
this object apart from a hard-float one"; exit 1; }
    # Enums are int here and -fshort-enums is refused, so the tag says 2.
    grep -A3 'TagName: ABI_enum_size' "$out/a.txt" | grep -qi 'int' || {
        echo "Tag_ABI_enum_size does not say int:"; cat "$out/a.txt"; exit 1; }
    echo "they decode, and report v7 / v7E-M, the soft-float ABI and int enums"
else
    echo "SKIP the decode half: no readelf that reads ARM attributes"
fi

# ---- -dumpmachine no longer answers for a different part --------------
got=$("$EMBCC" --target=thumbv7em-none-eabi -dumpmachine)
[ "$got" = "thumbv7em-none-eabi" ] || {
    echo "-dumpmachine answered '$got' for a thumbv7em request"; exit 1; }
got=$("$EMBCC" --target=thumbv7m-none-eabi -dumpmachine)
[ "$got" = "thumbv7m-none-eabi" ] || {
    echo "-dumpmachine answered '$got' for a thumbv7m request"; exit 1; }
echo "-dumpmachine answers for the part that was asked for"

# ---- the ARM machine flags every Cortex-M build passes ----------------
# Accepted where they describe what EmbCC does, refused BY NAME where
# they describe something else. The float ones are the point: guessing
# either way is the ABI mismatch above.
for fl in -mthumb -mcpu=cortex-m3 -mcpu=cortex-m4 -mfpu=none -mfloat-abi=soft \
          -mfpu=fpv4-sp-d16 "-mfpu=fpv4-sp-d16 -mfloat-abi=softfp" \
          "-mfpu=fpv4-sp-d16 -mfloat-abi=hard"
do
    "$EMBCC" --target=thumbv7em-none-eabi $fl -c "$out/t.c" -o /dev/null \
        2> "$out/f.err" || { echo "$fl was refused:"; cat "$out/f.err"
                             exit 1; }
done
# -mfloat-abi=softfp and =hard with no FPU named, and an FPU the part
# does not have (FPv5 is the Cortex-M33's). The ARMv6-M and ARMv8-M
# Baseline cores were accepted and given ARMv7-M code they fault on.
for fl in -mfloat-abi=hard -mfloat-abi=softfp -mfpu=fpv5-sp-d16 \
          -marm -mcpu=cortex-m9 -mcpu=cortex-m0 -mcpu=cortex-m0plus \
          -mcpu=cortex-m1 -mcpu=cortex-m23
do
    if "$EMBCC" --target=thumbv7em-none-eabi $fl -c "$out/t.c" -o /dev/null \
         2> "$out/f.err"; then
        echo "$fl was accepted, and EmbCC does not do it"; exit 1
    fi
    grep -q 'not supported\|not a part\|needs an FPU\|has no\|is not\|does not implement' "$out/f.err" || {
        echo "$fl's refusal does not say why:"; cat "$out/f.err"; exit 1; }
done
echo "the ARM machine flags are accepted where they match and refused by
name where they do not"

# ---- OUR linker refuses a mismatch -----------------------------------
# Against objects from another toolchain, since interoperating with one
# is what the tags are for. Skipped when that toolchain is absent: this
# checks EmbCC's linker, but it needs a counterexample to check it with.
LD=${EMBLD:-./embld}
if command -v clang > /dev/null 2>&1 &&
# A DIFFERENT symbol, so the duplicate-definition check does not fire
# first and hide the one being tested.
printf 'float hf(float a){ return a * 2.0f; }\n' > "$out/hf.c"
   clang --target=thumbv7em-none-eabihf -c "$out/hf.c" -o "$out/hard.o" \
        2>/dev/null; then
    if "$LD" -e f -Ttext 0x0 -o /dev/null "$out/thumbv7m-none-eabi.o" \
           "$out/hard.o" > "$out/ln.err" 2>&1; then
        echo "the linker accepted a soft-float and a hard-float object
together -- which links and then reads every float argument from a
register the caller never wrote"; exit 1
    fi
    grep -q 'floating-point arguments' "$out/ln.err" || {
        echo "the linker refused, but not for the float ABI:"
        head -3 "$out/ln.err"; exit 1; }
    echo "the linker refuses soft-float and hard-float objects together"

        printf 'enum E{A,B}; int se(enum E e){ return (int)e; }\n' \
            > "$out/se.c"
    if clang --target=thumbv7m-none-eabi -fshort-enums -c "$out/se.c" \
            -o "$out/short.o" 2>/dev/null; then
        if "$LD" -e f -Ttext 0x0 -o /dev/null \
               "$out/thumbv7m-none-eabi.o" "$out/short.o" \
               > "$out/ln2.err" 2>&1; then
            echo "the linker accepted disagreeing enum sizes"; exit 1
        fi
        grep -q 'size of an enum' "$out/ln2.err" || {
            echo "the linker refused, but not for the enum size:"
            head -3 "$out/ln2.err"; exit 1; }
        echo "...and objects that disagree about the size of an enum"
    fi
else
    echo "SKIP the linker half: no second toolchain to build a
counterexample with"
fi

# And a link of EmbCC's own objects, which all agree, still works.
"$EMBCC" --target=thumbv7m-none-eabi -c "$out/t.c" -o "$out/s1.o"
printf 'int f(int); int main(void){ return f(1); }\n' > "$out/m.c"
"$EMBCC" --target=thumbv7m-none-eabi -c "$out/m.c" -o "$out/s2.o"
"$LD" -e main -Ttext 0x0 -o "$out/img.elf" "$out/s1.o" "$out/s2.o" || {
    echo "a link of two agreeing EmbCC objects was refused"; exit 1; }
echo "and a link of objects that agree is unaffected"
