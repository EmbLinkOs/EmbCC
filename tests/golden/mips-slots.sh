#!/bin/sh
# MIPS32 delay-slot filling: the instruction before a transfer moves into
# its slot only when that keeps the program.
#
# tests/golden/mips-slots/slots.c puts, before a branch or a call, an
# instruction that must stay (it writes the branch's operand, saves $ra
# under a jal, writes jalr's target, or a label separates it from the
# branch -- the epilogue's, when another return branches there) or that
# may move (a call's argument setup). It runs on malta at
# -O1, -O2 and -Os; and the disassembly must show filled slots -- some jal
# with an instruction after it that is not a nop -- so the test cannot
# pass by filling nothing.
set -u
echo "TEST-MARKER mips-slots"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped: $QEMU not found"; exit 0; }
T=mipsel-none-elf
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/mips-slots
rm -rf "$out"; mkdir -p "$out"
export EMBCC_MIPS_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/mips/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
want="10 20 1 2 42 42 15 15 42 7"
for opt in -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c tests/golden/mips-slots/slots.c \
        -o "$out/s$opt.o" || { echo "$opt: slots.c does not compile"; exit 1; }
    if command -v "$OD" >/dev/null 2>&1; then
        filled=$("$OD" -d "$out/s$opt.o" | awk '
            /\tjal\t/ { j = 1; next }
            j { if ($0 !~ /\tnop/) n++; j = 0 }
            END { print n + 0 }')
        [ "$filled" -gt 0 ] || {
            echo "$opt: no call's delay slot is filled"; exit 1; }
    fi
    sh tests/harness/mips/link.sh "$out/s$opt.elf" "$out/s$opt.o" || {
        echo "$opt: does not link"; exit 1; }
    sh tests/harness/mips/run.sh "$out/s$opt.elf" > "$out/s$opt.out" 2>&1
    got=$(head -1 "$out/s$opt.out" | sed 's/ *$//')
    if [ "$got" != "$want" ] || ! grep -q '==EXIT 42 ==' "$out/s$opt.out"; then
        echo "$opt: wanted \"$want\", got:"; head -c 300 "$out/s$opt.out"; echo; exit 1
    fi
done
echo "filled delay slots keep the program at -O1, -O2 and -Os: branch operands,"
echo "\$ra under a jal, jalr's target and labels keep their instruction"
