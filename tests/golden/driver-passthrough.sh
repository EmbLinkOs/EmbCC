#!/bin/sh
# Options a build system hands the compiler for the TOOLS behind it:
# -Wl,/-Xlinker for the linker, -Wa, for the assembler, and -pedantic,
# which Makefiles pass out of habit. Each was refused as an unknown
# argument, which stopped those builds before they began.
#
# The linker's are applied when they are EmbLD's own (-e, -Ttext, -Tdata,
# -Tstack, --rom-limit, --lma-offset, -T, -L, -u, ...), accepted when they
# change nothing about an image EmbLD makes (--gc-sections, -z
# noexecstack, ...), and refused by name otherwise: a section start that
# was quietly dropped would build a different image from the one asked
# for, and a firmware image linked to the wrong memory map runs, wrongly.
set -u
echo "TEST-MARKER driver-passthrough"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/driver-passthrough-$ARCH
rm -rf "${out:?}"; mkdir -p "$out"
T=--target=x86_64-elf                  # freestanding: links what it is given
printf 'void _start(void) { for (;;); }\n' > "$out/s.c"

entry() { "$READELF" -h "$1" | sed -n 's/.*Entry point address: *//p'; }

# 1. -Wl, splits at the commas and each piece reaches the link.
"$EMBCC" $T "$out/s.c" -o "$out/a.elf" \
    -Wl,-e,_start,-Ttext=0x500000,--gc-sections || {
    echo "FAIL: -Wl,-e,_start,-Ttext=0x500000,--gc-sections was refused"; exit 1; }
[ "$(entry "$out/a.elf")" = 0x500000 ] || {
    echo "FAIL: -Wl,-Ttext=0x500000 did not place the text: entry $(entry "$out/a.elf")"
    exit 1; }

# 2. -Xlinker passes one word at a time; -Ttext's value is the next one.
"$EMBCC" $T "$out/s.c" -o "$out/b.elf" -Xlinker -Ttext -Xlinker 0x800000 \
    -Wl,-z,noexecstack || {
    echo "FAIL: -Xlinker -Ttext -Xlinker 0x800000 was refused"; exit 1; }
[ "$(entry "$out/b.elf")" = 0x800000 ] || {
    echo "FAIL: -Xlinker -Ttext did not place the text: entry $(entry "$out/b.elf")"
    exit 1; }
echo "-Wl, and -Xlinker options reach the link"

# 3. One EmbLD does not have is refused by name, and nothing is written.
for opt in -Wl,--section-start=.text=0 -Wl,--emit-relocs -Wl,-z,execstack; do
    rm -f "$out/c.elf"
    if err=$("$EMBCC" $T "$out/s.c" -o "$out/c.elf" "$opt" 2>&1); then
        echo "FAIL: $opt was accepted and dropped"; exit 1
    fi
    echo "$err" | grep -q "is not one EmbLD has" || {
        echo "FAIL: $opt refused without naming why:"; echo "$err"; exit 1; }
    [ ! -e "$out/c.elf" ] || { echo "FAIL: $opt refused but $out/c.elf written"; exit 1; }
done
# A linker script is EmbLD's, for a firmware image; an x86-64 one is laid
# out without, and says so rather than dropping it.
rm -f "$out/c.elf"
if err=$("$EMBCC" $T "$out/s.c" -o "$out/c.elf" -Wl,-T,link.ld 2>&1); then
    echo "FAIL: -Wl,-T for x86-64 was accepted"; exit 1
fi
echo "$err" | grep -q "a linker script (-T) is for an ARM or RISC-V image" || {
    echo "FAIL: -Wl,-T for x86-64 refused without naming why:"; echo "$err"; exit 1; }
[ ! -e "$out/c.elf" ] || { echo "FAIL: -Wl,-T refused but $out/c.elf written"; exit 1; }
# ...but a compile that does not link ignores them, as GCC's does.
"$EMBCC" $T -c "$out/s.c" -o "$out/s.o" -Wl,-T,link.ld || {
    echo "FAIL: -c refused a linker option it never uses"; exit 1; }
echo "and a linker option EmbLD lacks is refused by name"

# 4. The integrated assembler takes the options that change nothing it
#    produces, and refuses the rest.
"$EMBCC" $T -c "$out/s.c" -o "$out/s.o" -Wa,--noexecstack,-g || {
    echo "FAIL: -Wa,--noexecstack,-g was refused"; exit 1; }
if err=$("$EMBCC" $T -c "$out/s.c" -o "$out/s.o" -Wa,-mfoo 2>&1); then
    echo "FAIL: -Wa,-mfoo was accepted"; exit 1
fi
echo "$err" | grep -q "assembler option '-mfoo'" || {
    echo "FAIL: -Wa,-mfoo refused without naming it:"; echo "$err"; exit 1; }
echo "-Wa, takes the harmless options and refuses the rest by name"

# 5. -pedantic is accepted, and says that it turns nothing on.
err=$("$EMBCC" $T -c "$out/s.c" -o "$out/s.o" -pedantic 2>&1) || {
    echo "FAIL: -pedantic was refused"; exit 1; }
echo "$err" | grep -q "turns nothing on" || {
    echo "FAIL: -pedantic was accepted silently:"; echo "$err"; exit 1; }
echo "-pedantic is accepted, and says what it does"

# 6. A link embld refuses fails the command and leaves nothing behind:
#    embld's refusal ended the process, and the compile's temporary
#    object (OUT.embcc-tmp.o) stayed beside the output.
printf 'int undefined_fn(void);\nvoid _start(void) { undefined_fn(); for (;;); }\n' \
    > "$out/u.c"
if "$EMBCC" $T "$out/u.c" -o "$out/u.elf" 2> "$out/u.err"; then
    echo "FAIL: a link with an undefined symbol succeeded"; exit 1
fi
grep -q "undefined symbol 'undefined_fn'" "$out/u.err" || {
    echo "FAIL: the link failure does not name the symbol:"; cat "$out/u.err"; exit 1; }
[ ! -e "$out/u.elf.embcc-tmp.o" ] && [ ! -e "$out/u.elf" ] || {
    echo "FAIL: the failed link left $(ls "$out" | grep '^u\.elf' | tr '\n' ' ')"; exit 1; }
echo "and a refused link leaves no output and no temporary object"

# 7. The LAST -O wins, size mode included, as with GCC: -Os followed by
#    -O0 or -O2 kept optimizing for size.
printf 'int f(int *a) { int s = 0; for (int i = 0; i < 64; i++) s += a[i]; return s; }\n' \
    > "$out/o.c"
for f in "-O2" "-Os -O2" "-O0" "-Os -O0" "-Os"; do
    n=$(echo $f | tr -d ' -')
    # shellcheck disable=SC2086
    "$EMBCC" $T $f -c "$out/o.c" -o "$out/o$n.o" || {
        echo "FAIL: $f does not compile"; exit 1; }
done
cmp -s "$out/oOs.o" "$out/oO2.o" && {
    echo "FAIL: the probe compiles the same at -Os and -O2, so it proves nothing"; exit 1; }
cmp -s "$out/oO2.o" "$out/oOsO2.o" || { echo "FAIL: -Os -O2 is not -O2"; exit 1; }
cmp -s "$out/oO0.o" "$out/oOsO0.o" || { echo "FAIL: -Os -O0 is not -O0"; exit 1; }
echo "and the last -O wins, size mode included"
