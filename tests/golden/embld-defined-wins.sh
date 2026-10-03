#!/bin/sh
# A symbol the program defines is the program's, even when the linker
# would provide one of that name (_end, __bss_start, __init_array_start,
# ...).
#
# Only a definition IN A SECTION won. An absolute one (`.set _end, ADDR`)
# was replaced by the image's end, and a COMMON one (`int _end;` from a
# -fcommon compile) was too -- so a store to the program's own variable
# landed past the end of the image.
set -u
echo "TEST-MARKER embld-defined-wins"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/embld-defined-wins
rm -rf "${out:?}"; mkdir -p "$out"
command -v clang >/dev/null 2>&1 && command -v llvm-readelf >/dev/null 2>&1 || {
    echo "SKIP: needs clang (for an absolute and a COMMON symbol) and llvm-readelf"
    exit 0; }
LD=${EMBLD:-./embld}

printf '.globl _end\n.set _end, 0x123456\n' > "$out/abs.s"
clang --target=x86_64-elf -c "$out/abs.s" -o "$out/abs.o" || exit 1
printf 'int end_marker; int _end;\n' > "$out/cm.c"
clang --target=x86_64-elf -fcommon -c "$out/cm.c" -o "$out/cm.o" || exit 1
printf 'extern int _end;\nvoid _start(void) { _end = 7; for (;;) ; }\n' > "$out/use.c"
"$EMBCC" --target=x86_64-elf -c "$out/use.c" -o "$out/use.o" || exit 1

"$LD" -e _start -o "$out/abs.elf" "$out/use.o" "$out/abs.o" || exit 1
v=$(llvm-readelf -s "$out/abs.elf" | awk '$NF == "_end" { print $2 }')
[ "$v" = 0000000000123456 ] || {
    echo "FAIL: the object's absolute _end became $v"; exit 1; }

"$LD" -e _start -o "$out/cm.elf" "$out/use.o" "$out/cm.o" || exit 1
llvm-readelf -s "$out/cm.elf" > "$out/cm.sym"
e=$(awk '$NF == "_end" { print $2 }' "$out/cm.sym")
k=$(awk '$NF == "kernel_end" { print $2 }' "$out/cm.sym")
[ -n "$e" ] && [ $((0x$e + 4)) -le $((0x$k)) ] || {
    echo "FAIL: the COMMON _end at $e is not storage inside the image (end $k)"
    cat "$out/cm.sym"; exit 1; }
echo "embld-defined-wins: an absolute and a COMMON _end are the program's, not the linker's"
