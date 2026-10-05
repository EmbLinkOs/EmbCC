#!/bin/sh
# MIPS32 jump tables: a dense switch dispatches through a table of
# offsets from the address `bal` returns, with no relocation.
#
# tests/golden/mips-switch/sw.c has a switch in a leaf function, whose
# $ra must survive the bal, and one in a function that calls, both fed
# values below, inside (holes included) and above their range. Compiled
# at -O0..-Os, each object must contain the dispatch (a bal), and the
# program must print what the cases say on malta.
set -u
echo "TEST-MARKER mips-switch"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped: $QEMU not found"; exit 0; }
T=mipsel-none-elf
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/mips-switch
rm -rf "$out"; mkdir -p "$out"
export EMBCC_MIPS_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/mips/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
want="123984 200 200 2 4 6 8 10 200 14 16 200 200"
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c tests/golden/mips-switch/sw.c -o "$out/s$opt.o" ||
        { echo "$opt: sw.c does not compile"; exit 1; }
    if command -v "$OD" >/dev/null 2>&1; then
        n=$("$OD" -d "$out/s$opt.o" | grep -c '	bal	')
        [ "$n" -ge 2 ] || { echo "$opt: the switches are not jump tables"; exit 1; }
    fi
    sh tests/harness/mips/link.sh "$out/s$opt.elf" "$out/s$opt.o" || {
        echo "$opt: does not link"; exit 1; }
    sh tests/harness/mips/run.sh "$out/s$opt.elf" > "$out/s$opt.out" 2>&1
    got=$(head -1 "$out/s$opt.out" | sed 's/ *$//')
    if [ "$got" != "$want" ] || ! grep -q '==EXIT 42 ==' "$out/s$opt.out"; then
        echo "$opt: wanted \"$want\", got:"; head -c 300 "$out/s$opt.out"; echo
        exit 1
    fi
done
echo "dense switches dispatch through a jump table at -O0..-Os, in a leaf"
echo "function and in one that calls, and land on the right case"
