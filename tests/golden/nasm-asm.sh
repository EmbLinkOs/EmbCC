#!/bin/sh
# embas, EmbCC's NASM-syntax assembler (src/as/nasm.c), against nasm:
#
#   1. nasm/boot.asm -- a boot sector from real mode to long mode, 16-,
#      32- and 64-bit code in one -f bin image (org, times 510-($-$$), a
#      far jump, lgdt, `` strings) -- byte for byte nasm's; and, with nasm
#      or without, 512 bytes ending 55 AA;
#   2. nasm/pp.asm -- the preprocessor (%macro with defaults, greedy and
#      %rotate, %rep/%assign, %if/%elif, %ifidn, %strlen, %xdefine,
#      %include) and the directives (struc/istruc, equ, align, incbin,
#      common, global with a type and size, wrt ..plt, default rel) in an
#      -f elf64 object: each section's bytes, flags and alignment, the
#      relocations and the symbols as nasm's (a macro's ..@ labels aside);
#   3. -D, and embcc -c FILE.asm the same as embas;
#   4. misuses refused by name.
set -u
echo "TEST-MARKER nasm-asm"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
EMBAS=${EMBAS:-$PWD/embas}
case $EMBCC in /*) ;; *) EMBCC=$PWD/$EMBCC ;; esac
case $EMBAS in /*) ;; *) EMBAS=$PWD/$EMBAS ;; esac
here=$PWD/tests/golden/nasm
out=$PWD/tests/golden/out/nasm-asm
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBAS" ] || fail "no embas (make embas)"
NASM=$(command -v nasm || true)

# ---- 1 ----
(cd "$here" && "$EMBAS" -f bin boot.asm -o "$out/boot.bin") || fail "embas refused boot.asm"
[ "$(wc -c < "$out/boot.bin" | tr -d ' ')" = 512 ] || fail "the boot sector is not 512 bytes"
[ "$(od -An -tx1 -j510 "$out/boot.bin" | tr -d ' \n')" = 55aa ] || fail "no 55 AA at its end"
if [ -n "$NASM" ]; then
    (cd "$here" && "$NASM" -f bin boot.asm -o "$out/boot.ref.bin") || fail "nasm refused boot.asm"
    cmp -s "$out/boot.bin" "$out/boot.ref.bin" || {
        cmp -l "$out/boot.bin" "$out/boot.ref.bin" | head -5
        fail "boot.asm: the image differs from nasm's"; }
    echo "a boot sector, 16- to 64-bit code: nasm's image byte for byte"
else
    echo "  (no nasm: the image's size and signature only)"
fi

# ---- 2 ----
(cd "$here" && "$EMBAS" -f elf64 pp.asm -o "$out/pp.o") || fail "embas refused pp.asm"
view() {
    llvm-readelf -S "$1" | awk '/^ +\[ *[0-9]+\]/ { sub(/^ +\[ *[0-9]+\] */, "");
        if ($1 !~ /^\.(rela|symtab|strtab|shstrtab)/ && $1 != "NULL") print "S", $1, $2, $6, $7, $NF }'
    for s in .text .data .rodata; do
        echo "B $s $(llvm-objcopy -O binary -j $s "$1" /dev/stdout 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    done
    llvm-objdump -r "$1" | awk '/RELOCATION RECORDS FOR/ { s = $4 } /R_X86/ { print "R", s, $1, $2, $3 }'
    llvm-objdump -t "$1" | awk '/^[0-9a-f]+ / && $NF !~ /^(\.L|\.\.@)/ && $3 != "df" { print "Y", $0 }' |
        grep -v ' d  *\.'
}
if [ -n "$NASM" ] && command -v llvm-readelf >/dev/null; then
    (cd "$here" && "$NASM" -f elf64 pp.asm -o "$out/pp.ref.o") || fail "nasm refused pp.asm"
    view "$out/pp.o" | sort > "$out/pp.view"
    view "$out/pp.ref.o" | sort > "$out/pp.ref.view"
    diff "$out/pp.ref.view" "$out/pp.view" > "$out/pp.diff" ||
        fail "pp.asm: the object differs from nasm's (< nasm, > embas):
$(head -12 "$out/pp.diff")"
    [ "$(grep -c '^R' "$out/pp.view")" -ge 8 ] || fail "pp.asm: the relocations were not read"
    echo "the preprocessor and directives: nasm's sections, relocations and symbols"
fi

# ---- 3 ----
(cd "$here" && "$EMBAS" -f bin -DLOAD_SECTORS=9 boot.asm -o "$out/boot9.bin") || fail "-D"
cmp -s "$out/boot.bin" "$out/boot9.bin" && fail "-DLOAD_SECTORS=9 changed nothing"
if [ -n "$NASM" ]; then
    (cd "$here" && "$NASM" -f bin -DLOAD_SECTORS=9 boot.asm -o "$out/boot9.ref.bin")
    cmp -s "$out/boot9.bin" "$out/boot9.ref.bin" || fail "-D: the image differs from nasm's"
fi
(cd "$here" && "$EMBCC" -c pp.asm -o "$out/via.o") || fail "embcc -c pp.asm"
cmp -s "$out/pp.o" "$out/via.o" || fail "embcc -c FILE.asm differs from embas"
echo "-D, and embcc -c FILE.asm as embas"

# ---- 4 ----
refs() {
    printf '%b\n' "$2" > "$out/bad.asm"
    if "$EMBAS" $3 "$out/bad.asm" -o "$out/bad.o" 2> "$out/bad.err"; then
        fail "$1 was accepted"
    fi
    grep -q -- "$4" "$out/bad.err" || fail "$1 was refused, but not by name: $(cat "$out/bad.err")"
}
refs "org in an object" 'org 0x100\nnop' "-f elf64" "org is a -f bin directive"
refs "an extern in a flat image" 'extern f\ncall f' "-f bin" "no linker"
refs "an unclosed %if" '%if 1\nnop' "-f elf64" "%if with no %endif"
refs "an unknown directive" '%frobnicate x' "-f elf64" "not a directive"
refs "rax in 16-bit code" 'bits 16\nmov rax, 1' "-f bin" "only in 64-bit code"
refs "%error" '%error stop here' "-f elf64" "stop here"
"$EMBAS" -f elf32 "$out/bad.asm" -o "$out/bad.o" 2> "$out/bad.err" && fail "-f elf32 was accepted"
grep -q "elf64 objects and bin images" "$out/bad.err" || fail "-f elf32 refused, but not by name"
echo "seven misuses refused by name"
