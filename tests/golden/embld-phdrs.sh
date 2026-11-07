#!/bin/sh
# embld's PHDRS: a script's own program headers, as GNU ld writes them --
# the segments in the order declared, each section in the ones `:name`
# puts it in (and the sections after it, until another `:name`), a
# PT_LOAD's sections at file offsets congruent to their addresses modulo
# the page, FILEHDR/PHDRS loading the headers, PT_PHDR, PT_TLS, PT_NOTE,
# an empty PT_GNU_STACK with FLAGS, and SIZEOF_HEADERS counting them;
# .tbss taking no address space, and a .note output section of type
# NOTE. Against x86_64-elf-ld when it is here: the program headers and
# the section headers alike. user.ld is an operating system's user
# program script (two PT_LOADs, R+X and R+W).
set -u
echo "TEST-MARKER embld-phdrs"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
EMBLD=${EMBLD:-$PWD/embld}
here=$PWD/tests/golden/embld-phdrs
out=$PWD/tests/golden/out/embld-phdrs
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
LD=$(command -v x86_64-elf-ld || true)

"$EMBCC" --target=x86_64-elf -O2 -c "$here/v.c" -o "$out/v.o" || fail "v.c"
"$EMBCC" --target=x86_64-elf -O2 -c "$here/u.c" -o "$out/u.o" || fail "u.c"
view() {
    llvm-readelf -lW "$1" | grep -E '^  [A-Z]' > "$1.ph"
    llvm-readelf -SW "$1" | grep -E '^ +\[' | awk '{print $2, $3, $4, $5}' |
        grep -v 'symtab\|strtab' > "$1.sec"
}
for spec in full:v user:u; do
    s=${spec%:*}; o=${spec#*:}
    "$EMBLD" -T "$here/$s.ld" "$out/$o.o" -o "$out/$s.elf" 2> "$out/$s.err" ||
        fail "embld refused $s.ld: $(head -3 "$out/$s.err")"
    view "$out/$s.elf"
    if [ -n "$LD" ]; then
        "$LD" -T "$here/$s.ld" "$out/$o.o" -o "$out/$s.ref.elf" || fail "ld refused $s.ld"
        view "$out/$s.ref.elf"
        diff "$out/$s.ref.elf.ph" "$out/$s.elf.ph" > "$out/$s.diff" ||
            fail "$s.ld: the program headers differ from ld's: $(cat "$out/$s.diff")"
        diff "$out/$s.ref.elf.sec" "$out/$s.elf.sec" > "$out/$s.diff" ||
            fail "$s.ld: the sections differ from ld's: $(cat "$out/$s.diff")"
    fi
done
grep -q '^  PHDR ' "$out/full.elf.ph" && grep -q '^  TLS ' "$out/full.elf.ph" &&
    grep -q '^  GNU_STACK ' "$out/full.elf.ph" || fail "full.ld: a program header is missing"
[ "$(grep -c '^  LOAD ' "$out/user.elf.ph")" = 2 ] || fail "user.ld: not two PT_LOADs"
if [ -n "$LD" ]; then
    echo "PHDRS: ld's program headers and sections, for two scripts"
else
    echo "PHDRS: the segments the scripts declare (no x86_64-elf-ld to compare)"
fi

printf 'PHDRS { text PT_LOAD; }\nSECTIONS { .text : { *(.text) } :nosuch }\n' > "$out/bad.ld"
"$EMBLD" -T "$out/bad.ld" "$out/v.o" -o "$out/bad.elf" 2> "$out/bad.err" &&
    fail "a section in an undeclared program header was linked"
grep -q "which PHDRS does not declare" "$out/bad.err" || fail "refused, but not by name"
echo "an undeclared program header is refused by name"
