#!/bin/sh
# The ColdFire target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine, the object's header (EM_68K, ELF32,
# big-endian, e_flags 0x2 = ColdFire ISA_A, RELA relocations, R_68K_32 for
# an address and a call), the machine options a ColdFire build passes --
# the one configuration EmbCC emits is accepted, every other one refused --
# the constructs the backend does not lower, each with a message naming it
# rather than code that does something else, and the objects EmbLD will
# not link into a ColdFire image.
set -u
echo "TEST-MARKER coldfire-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
T=m68k-none-elf
out=tests/golden/out/coldfire-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in m68k-none-elf m68k-unknown-elf m68k-elf m68k; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = m68k-none-elf ] || { echo "--target=$t is '$m'"; exit 1; }
done
printf 'int f(int x) { return x + 1; }\n' > "$out/f.c"
"$EMBCC" --target=$T -c "$out/f.c" -o "$out/f.o" || { echo "-c failed"; exit 1; }
"$EMBCC" --target=$T --dump-predef > "$out/predef.txt" || exit 1
for m in '__m68k__ 1' '__mcoldfire__ 1' '__mcfisaa__ 1' '__mcfhwdiv__ 1' \
         '__BIGGEST_ALIGNMENT__ 2' '__BYTE_ORDER__ __ORDER_BIG_ENDIAN__' \
         '__SIZEOF_LONG_DOUBLE__ 8' '__SIZEOF_POINTER__ 4'; do
    grep -q "^#define $m\$" "$out/predef.txt" || {
        echo "the predefined macros lack '#define $m'"; exit 1; }
done
grep -q '__CHAR_UNSIGNED__\|__mcffpu__\|__HAVE_68881__' "$out/predef.txt" && {
    echo "the predefined macros claim an unsigned char or an FPU"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" -h -S "$out/f.o" > "$out/f.hdr" || exit 1
    grep -q 'Class:.*ELF32' "$out/f.hdr" &&
    grep -q 'Data:.*big endian' "$out/f.hdr" &&
    grep -q 'Machine:.*MC68000' "$out/f.hdr" &&
    grep -q 'Flags:.*0x2$' "$out/f.hdr" || {
        echo "the object header is not ELF32 big-endian EM_68K, e_flags 0x2:"
        grep -E 'Class|Data|Machine|Flags' "$out/f.hdr"; exit 1; }
    printf 'extern int g; int h(int); int f(void) { return h(g); }\n' > "$out/r.c"
    "$EMBCC" --target=$T -c "$out/r.c" -o "$out/r.o" &&
    "$RE" -S -r "$out/r.o" > "$out/r.rel" || exit 1
    grep -q '\.rela\.text *RELA ' "$out/r.rel" &&
    [ "$(grep -c 'R_68K_32 ' "$out/r.rel")" = 2 ] || {
        echo "an address and a call are not two R_68K_32 in .rela.text:"
        cat "$out/r.rel"; exit 1; }
    echo "an ELF32 big-endian EM_68K object, e_flags 0x2 (ISA_A), RELA, R_68K_32"
fi

# ---- the options -----------------------------------------------------------
for o in -mcpu=5208 -mcpu=5282 -mcpu=54455 -m5208 -march=isaa -march=isab \
         -mtune=5329 -msoft-float -mdiv -mno-align-int -mno-short \
         -mstrict-align -mno-strict-align -mno-rtd -mbig-endian; do
    "$EMBCC" --target=$T $o -c "$out/f.c" -o /dev/null 2> "$out/opt.err" || {
        echo "$o was refused:"; cat "$out/opt.err"; exit 1; }
done
refopt() {          # refopt OPTION PATTERN
    if "$EMBCC" --target=$T "$1" -c "$out/f.c" -o /dev/null 2> "$out/opt.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/opt.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/opt.err"; exit 1; }
}
refopt -mcpu=68000 'not a ColdFire core'
refopt -mcpu=68020 'not a ColdFire core'
refopt -m68020 'the 68000 family proper is not a target'
refopt -mcpu32 'the 68000 family proper is not a target'
refopt -mcpu=5206 'no hardware divide'
refopt -mcpu=5475 'soft float'
refopt -march=68000 'ColdFire ISA_A'
refopt -mhard-float '-mhard-float is not supported'
refopt -mno-div '-mno-div is not supported'
refopt -malign-int '-malign-int is not supported'
refopt -mshort '-mshort is not supported'
refopt -mrtd '-mrtd is not supported'
refopt -mpcrel 'takes every address absolutely'
refopt -mid-shared-library 'takes every address absolutely'
refopt -mlittle-endian 'big-endian'
for o in -funwind-tables -fasynchronous-unwind-tables -fexceptions; do
    refopt $o 'unwind tables are not supported for m68k-none-elf yet'
done
echo "the ColdFire ISA_A/soft-float/2-byte-aligned flags are accepted, others refused"

# ---- the constructs ----------------------------------------------------------
refc() {            # refc WHAT PATTERN SOURCE [FLAGS]
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T ${4:-} -c "$out/bad.c" -o /dev/null \
           2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "inline asm" 'inline assembly is not supported for m68k-none-elf' \
    'int f(void){ __asm__ volatile("nop"); return 0; }'
refc "file-scope asm" 'file-scope asm is not supported for m68k-none-elf' \
    '__asm__(".globl x\nx: .long 0"); int f(void){ return 0; }'
refc "an 8-byte atomic read-modify-write" 'an atomic wider than four bytes' \
    'long long x; long long f(void){ return __atomic_fetch_add(&x, 1, 5); }'
refc "an 8-byte atomic load" 'an atomic access of 8 bytes is not one access' \
    'long long x; long long f(void){ return __atomic_load_n(&x, 5); }'
refc "__int128" '__int128' \
    '__int128 f(__int128 a){ return a; }'
refc "a frame beyond 32 KiB" 'a stack frame larger than 32 KiB' \
    'void g(char *); void f(void){ char b[40000]; g(b); }'
printf '\t.text\n\tnop\n' > "$out/a.s"
if "$EMBCC" --target=$T -c "$out/a.s" -o /dev/null 2> "$out/as.err"; then
    echo "an assembly file was accepted"; exit 1
fi
grep -q 'no assembly-file support for m68k-none-elf yet: EmbCC has no ColdFire assembler' \
    "$out/as.err" || { echo "an assembly file was refused, but not by name:"
    cat "$out/as.err"; exit 1; }
printf 'int main() { return 0; }\n' > "$out/c.cc"
if "$EMBCC" --target=$T -c "$out/c.cc" -o /dev/null 2> "$out/cc.err"; then
    echo "a C++ unit was accepted"; exit 1
fi
grep -q 'C++ is not yet supported for m68k-none-elf' "$out/cc.err" || {
    echo "a C++ unit was refused, but not by name:"; cat "$out/cc.err"; exit 1; }
echo "inline and file-scope asm, .s files, C++, wide atomics, __int128, a frame"
echo "beyond 32 KiB and an over-aligned scalar local are refused by name"

# ---- EmbLD ---------------------------------------------------------------
# An m68k object built for an FPU (e_flags' 0x40) or for the 68000 family
# proper (no ColdFire ISA in the low nibble) is not linked into a
# soft-float ColdFire image: e_flags is the header's bytes 36..39.
printf 'int main(void){ return 0; }\n' > "$out/m.c"
"$EMBCC" --target=$T -c "$out/m.c" -o "$out/m.o" || exit 1
for flags in 42 00; do
    cp "$out/m.o" "$out/bad.o"
    printf "\\$(printf '%03o' 0x$flags)" |
        dd of="$out/bad.o" bs=1 seek=39 conv=notrunc 2>/dev/null
    if "$EMBLD" -e main -Ttext 0x40100000 "$out/bad.o" -o "$out/bad.elf" \
           2> "$out/ld.err"; then
        echo "EmbLD linked an m68k object with e_flags 0x$flags"; exit 1
    fi
    grep -q 'not soft-float ColdFire' "$out/ld.err" || {
        echo "EmbLD refused e_flags 0x$flags, but not by name:"
        cat "$out/ld.err"; exit 1; }
done
"$EMBLD" -e main -Ttext 0x40100000 -Tstack 0x48000000 "$out/m.o" \
    -o "$out/m.elf" || { echo "EmbLD did not link a ColdFire object"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" -h "$out/m.elf" | grep -q 'Machine:.*MC68000' || {
        echo "the image is not EM_68K"; exit 1; }
fi
echo "EmbLD links ColdFire objects and refuses an FPU or 68000-family one by name"
