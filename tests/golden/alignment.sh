#!/bin/sh
# An object's alignment has to survive into the OBJECT, not just the
# layout inside it.
#
# It did not. The section headers carried fixed numbers -- .data and
# .bss said 8, .rodata said 1 -- and `__attribute__((aligned(N)))` on a
# file-scope object was dropped entirely, because struct global had
# nowhere to put it. So an object was laid out correctly WITHIN its
# section, the linker was told the section needed only eight bytes, and
# it placed the section wherever that allowed.
#
# Nothing failed. The address was simply not the one that was asked
# for, which for a page table is the whole point of asking.
#
# The reference is gcc: it is the authority on what the same source
# should produce, and comparing against ourselves would have agreed
# with the bug.
set -eu
echo "TEST-MARKER alignment"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/alignment
rm -rf "$out"; mkdir -p "$out"

case "$ARCH" in
    x86_64)  RE=x86_64-elf-readelf;  GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc} ;;
    aarch64) RE=aarch64-elf-readelf; GCC=aarch64-elf-gcc ;;
    *) echo "skipped: no binutils for $ARCH"; exit 0 ;;
esac
command -v "$RE" > /dev/null 2>&1 || { echo "skipped: no $RE"; exit 0; }

cat > "$out/a.c" << 'EOF'
/* One of each placement: uninitialised, initialised, and static. A
 * page-aligned array is the case that matters -- it is what a kernel's
 * page tables are -- and 64 is the cache-line case. */
int pg[512]  __attribute__((aligned(4096)));
static int spg[512] __attribute__((aligned(4096)));
int d64[16]  __attribute__((aligned(64))) = { 1 };
_Alignas(128) int ali[4];
EOF
"$EMBCC" --target="$TARGET" -c "$out/a.c" -o "$out/a.o" 2> "$out/cc.log" || {
    echo "FAIL: did not compile:"; cat "$out/cc.log"; exit 1; }

# A static LOCAL is the same object with a different name. It has static
# storage, so `_Alignas` on it means the same thing it means at file
# scope -- but it reaches the object by a different path (sema promotes
# it to a global of its own), and that path dropped the field. Two
# orderings, because the specifiers are unordered in C11 and only one of
# them used to parse at all: `_Alignas(64) static` crashed the compiler,
# because parse_type_spec ate the alignment, found `static` where a type
# should be, and returned NULL to a caller that dereferenced it.
cat > "$out/s.c" << 'EOF'
int f(void)
{
    _Alignas(64) static char a[1];        /* alignment first */
    static _Alignas(64) char b[1];        /* storage first */
    _Alignas(64) static char c[1] = { 1 };/* and in .data */
    return a[0] + b[0] + c[0];
}
EOF
"$EMBCC" --target="$TARGET" -c "$out/s.c" -o "$out/s.o" 2> "$out/s.log" || {
    echo "FAIL: a static local with _Alignas did not compile:"
    cat "$out/s.log"; exit 1; }

secalign() {                       # secalign FILE SECTION -> alignment
    # readelf prints "[ 2] .bss ... 4096", so the name is not a fixed
    # field: the index is bracketed and splits differently depending on
    # how many sections there are. Match the name as any field and take
    # the last, which is the alignment column.
    "$RE" -SW "$1" | awk -v s="$2" \
        '{ for (i = 1; i <= NF; i++) if ($i == s) { print $NF; exit } }'
}

for sec in .bss .data; do
    got=$(secalign "$out/a.o" "$sec")
    [ -n "$got" ] || { echo "FAIL: no $sec in the object"; exit 1; }
    [ "$got" -ge 64 ] || {
        echo "FAIL: $sec claims alignment $got, and the file asks for"
        echo "      4096 (.bss) / 64 (.data) -- the attribute was dropped"
        "$RE" -SW "$out/a.o" | grep -E "Name|$sec"; exit 1; }
done
bss=$(secalign "$out/a.o" .bss)
[ "$bss" = 4096 ] || {
    echo "FAIL: .bss claims $bss, wanted 4096"; exit 1; }

# The static locals: both sections carry it, and -- the part a section
# header cannot show -- the two 1-byte arrays are actually 64 apart, so
# the alignment was applied to the LAYOUT and not just announced.
for sec in .bss .data; do
    got=$(secalign "$out/s.o" "$sec")
    [ "$got" = 64 ] || {
        echo "FAIL: a static local asks for _Alignas(64) and $sec claims"
        echo "      $got -- the attribute was dropped promoting the local"
        echo "      to a global"; exit 1; }
done
off_b=$("$RE" -sW "$out/s.o" | awk '$8 ~ /\.b$/ { print $2; exit }')
case "$off_b" in
    *[!0-9a-fA-F]* | "") echo "FAIL: no static local 'b' in the object"; exit 1 ;;
esac
[ $((0x$off_b % 64)) -eq 0 ] || {
    echo "FAIL: the second static local sits at 0x$off_b, which is not"
    echo "      64-aligned -- the section header says 64 and the layout"
    echo "      inside it does not agree"; exit 1; }
echo "a static local carries _Alignas in both orders, into both sections"

#  gcc, on the same source, as the authority. Skipped rather than
#  failed where the cross compiler is absent.
if command -v "$GCC" > /dev/null 2>&1; then
    "$GCC" -c "$out/a.c" -o "$out/g.o" 2>/dev/null
    for sec in .bss .data; do
        ours=$(secalign "$out/a.o" "$sec")
        theirs=$(secalign "$out/g.o" "$sec")
        [ "$ours" = "$theirs" ] || {
            echo "FAIL: $sec alignment is $ours here and $theirs in gcc's"
            echo "      object for the same source"; exit 1; }
    done
    echo "section alignment matches gcc's for the same source"
else
    echo "section alignment carries the attribute (no $GCC to compare)"
fi

# ---- and it survives the LINK ---------------------------------------------
#
# The section header is only half of it: the linker has to honour what
# it says. Linked after an object whose .bss is three bytes -- so the
# location counter is at an awkward offset when the aligned one arrives
# -- and then RUN, because the only conclusive check is the address the
# program actually sees.
[ "$ARCH" = x86_64 ] || {
    echo "the link half is x86-64 (EmbLD reads x86-64 ELF)"; exit 0; }
LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
[ -f "$LIBDIR/libc.a" ] || {
    echo "skipped the link half: no $LIBDIR/libc.a"; exit 0; }

printf 'char pad[3];\n' > "$out/pad.c"
cat > "$out/run.c" << 'EOF'
#include <stdio.h>
int pg[512] __attribute__((aligned(4096)));
static int spg[512] __attribute__((aligned(4096)));
int d64[16] __attribute__((aligned(64))) = { 1 };
int main(void)
{
    unsigned long bad = ((unsigned long)pg & 4095) |
                        ((unsigned long)spg & 4095) |
                        ((unsigned long)d64 & 63);
    printf("misaligned-bits=%lu\n", bad);
    return bad ? 1 : 42;
}
EOF
"$EMBCC" --target=x86_64-linux-gnu -c "$out/pad.c" -o "$out/pad.o"
"$EMBCC" --target=x86_64-linux-gnu -c "$out/run.c" -o "$out/run.o"
"$EMBCC_ROOT/embld" -o "$out/run.bin" "$LIBDIR/crt1.o" "$out/pad.o" \
    "$out/run.o" "$LIBDIR/libc.a" 2> "$out/ld.log" || {
    echo "FAIL: linking:"; cat "$out/ld.log"; exit 1; }

if "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1
then
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/run.bin" \
        > "$out/run.txt" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || {
        echo "FAIL: the linked program found its objects misaligned:"
        cat "$out/run.txt"; exit 1; }
    echo "and it survives the link: a 4096-aligned array placed after a
three-byte .bss is still 4096-aligned when the program reads its own
address"
else
    echo "linked; running it needs a kernel for tests/harness/linux"
fi
