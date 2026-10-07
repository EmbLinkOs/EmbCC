#!/bin/sh
# EmbLD's MIPS (o32) relocations, on the board and by refusal.
#
# o32 objects are REL: the addend is in the field. A HI16 and the LO16
# after it carry ONE addend between them -- AHL, the HI16 field shifted up
# plus the LO16 field sign-extended -- and the high half of the result is
# rounded by 0x8000 because the low half is sign-extended when it is added
# (docs/internals/mips32-plan.md). A linker that reads the halves apart,
# or forgets the rounding, is right for most addresses and 64 KB out for
# the rest, so this takes the cases one at a time:
#
#  1. llvm-mc assembles lui %hi(tgt+K) / addiu %lo(tgt+K) pairs (and a
#     load with %lo, and two HI16s sharing one LO16) for addends K on both
#     sides of every carry, against an ABSOLUTE symbol tgt whose low half
#     is chosen -- 0x0000, 0x7ffc, 0x8000, 0xfffc and others -- and the
#     program on the board compares each with tgt + K computed at run
#     time from an R_MIPS_32 pointer to tgt.
#  2. EmbCC's own addends: a string literal more than 32 KB into .rodata,
#     whose LO16 field the object writer stored sign-extended.
#  3. What is refused, by name: gp-relative small data, a hard-float
#     object linked with soft-float ones, a HI16 with no LO16, and a
#     linker script (-T), which EmbLD lays out for ARM and RISC-V only.
set -u
# Run BIG-endian (mips-none-elf) as tests/golden/mips-be-link.sh, which sets
# MIPS_BE=1.
if [ "${MIPS_BE:-0}" = 1 ]; then
    NAME=mips-be-link T=mips-none-elf MT=mips-unknown-elf
    QEMU=${EMBCC_QEMU_MIPSEB:-qemu-system-mips}
else
    NAME=mips-link T=mipsel-none-elf MT=mipsel-unknown-elf
    QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
fi
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped: $QEMU not found"; exit 0; }
command -v "$MC" >/dev/null 2>&1 || { echo "skipped: llvm-mc not found"; exit 0; }
"$MC" -triple=$MT -mcpu=mips32r2 /dev/null -o /dev/null \
    2>/dev/null || { echo "skipped: this llvm-mc has no MIPS target"; exit 0; }

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out"
export EMBCC_MIPS_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/mips/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
mc() { "$MC" -triple=$MT -mcpu=mips32r2 -mattr=+soft-float \
           -filetype=obj "$@"; }

# ---- 1. the AHL rule ------------------------------------------------------
KS="0 4 0x7ffc 0x8000 0x8ca0 0xfffc 0x10000 0x18000 -4 -0x8000 -0x8004 0x12344"
{
    echo '.text'; echo '.set noreorder'; echo '.set noat'
    n=0
    for k in $KS; do
        echo ".globl ahl_$n"
        echo "ahl_$n: lui \$2, %hi(tgt+$k)"
        echo "    jr \$ra"
        echo "    addiu \$2, \$2, %lo(tgt+$k)"
        n=$((n + 1))
    done
    # a word loaded through %lo: the LO16 is a load's offset
    echo '.globl ahl_load'
    echo "ahl_load: lui \$2, %hi(tgt+0x8ca0)"
    echo "    jr \$ra"
    echo "    lw \$2, %lo(tgt+0x8ca0)(\$2)"
    # two HI16s and the one LO16 that completes both
    echo '.globl ahl_two'
    echo "ahl_two: lui \$2, %hi(tgt+0x9000)"
    echo "    lui \$3, %hi(tgt+0x9000)"
    echo "    addu \$2, \$2, \$3"
    echo "    jr \$ra"
    echo "    addiu \$2, \$2, %lo(tgt+0x9000)"
} > "$out/ahl.s"
mc "$out/ahl.s" -o "$out/ahl.o" || { echo "llvm-mc rejected ahl.s"; exit 1; }
{
    echo '/* each ahl_n() against tgt + K computed at run time */'
    echo 'extern char tgt[]; extern void putn(long); extern void puts_(const char *);'
    echo 'static char *volatile base = tgt;'
    n=0
    for k in $KS; do echo "unsigned ahl_$n(void);"; n=$((n + 1)); done
    echo 'unsigned ahl_load(void); unsigned ahl_two(void);'
    echo 'int main(void) {'
    n=0
    for k in $KS; do
        echo "    putn(ahl_$n() == (unsigned)base + (unsigned)($k));"
        n=$((n + 1))
    done
    echo '    putn(ahl_two() == ((unsigned)base + 0x9000 + 0x8000) / 65536 * 65536'
    echo '                      + (unsigned)base + 0x9000);'
    printf '%s\n' '    puts_("\n==END==\n"); return 0; }'
} > "$out/ahl-main.c"
"$EMBCC" --target=$T -O2 -c "$out/ahl-main.c" -o "$out/ahl-main.o" || {
    echo "ahl-main.c does not compile"; exit 1; }
want=$(echo $KS x | awk '{ for (i = 1; i <= NF; i++) printf "1 "; }')
for v in 0x12340000 0x12341234 0x12347ffc 0x12348000 0x1234c000 0x1234fffc; do
    printf '.globl tgt\n.set tgt, %s\n' $v > "$out/tgt.s"
    mc "$out/tgt.s" -o "$out/tgt.o" || { echo "llvm-mc rejected tgt.s"; exit 1; }
    sh tests/harness/mips/link.sh "$out/ahl-$v.elf" "$out/ahl-main.o" \
        "$out/ahl.o" "$out/tgt.o" || { echo "tgt=$v: embld could not link"; exit 1; }
    sh tests/harness/mips/run.sh "$out/ahl-$v.elf" > "$out/ahl-$v.txt" 2>&1
    got=$(head -1 "$out/ahl-$v.txt")
    [ "$got" = "$want" ] || {
        echo "tgt=$v: a HI16/LO16 pair resolved to the wrong address"
        echo "(each 1 is one K in: $KS, then two HI16s and one LO16):"
        echo "  got  $got"; echo "  want $want"; exit 1; }
done
# the load through %lo, at a tgt that is real data: the image's own array
{
    echo 'extern void putn(long); extern void puts_(const char *);'
    echo 'unsigned ahl_load(void);'
    echo 'unsigned tgt[0x2400] = { [0x8ca0 / 4] = 0x5eed };'
    printf '%s\n' 'int main(void) { putn(ahl_load() == 0x5eed); puts_("\n==END==\n"); return 0; }'
} > "$out/load-main.c"
"$EMBCC" --target=$T -O2 -c "$out/load-main.c" -o "$out/load-main.o" &&
sh tests/harness/mips/link.sh "$out/load.elf" "$out/load-main.o" \
    "$out/ahl.o" > "$out/load.lerr" 2>&1 || {
    echo "the %lo load could not be built or linked:"; head -3 "$out/load.lerr"
    exit 1; }
sh tests/harness/mips/run.sh "$out/load.elf" > "$out/load.txt" 2>&1
grep -q '^1 $' "$out/load.txt" || {
    echo "a load through %lo(tgt+0x8ca0) read the wrong word:"
    head -3 "$out/load.txt"; exit 1; }
echo "HI16/LO16 pairs resolve by the AHL rule across every carry, six"
echo "symbol values and twelve addends, and through a load's offset"

# ---- 2. EmbCC's own addends -------------------------------------------------
{
    echo 'extern void puts_(const char *);'
    # (in a function, so irgen interns it first and the next literal
    # lands past it: a global's initializer is interned after the code)
    printf 'const char *pad(void) { return "'
    i=0; while [ $i -lt 340 ]; do
        printf '0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789'
        i=$((i + 1)); done
    echo '"; }'
    printf '%s\n' 'int main(void) { puts_(pad() + 33990); puts_(" far string\n==END==\n"); return 0; }'
} > "$out/far.c"
"$EMBCC" --target=$T -O2 -c "$out/far.c" -o "$out/far.o" &&
sh tests/harness/mips/link.sh "$out/far.elf" "$out/far.o" || {
    echo "the far-string program does not build"; exit 1; }
sh tests/harness/mips/run.sh "$out/far.elf" > "$out/far.txt" 2>&1
grep -q '^0123456789 far string$' "$out/far.txt" || {
    echo "a string past 32 KB of .rodata came out wrong:"
    head -3 "$out/far.txt"; exit 1; }
echo "a string 33 KB into .rodata is addressed right through EmbCC's own REL addends"

# ---- 3. refusals ------------------------------------------------------------
refuse() {          # refuse WHAT PATTERN OBJ...
    what=$1; pat=$2; shift 2
    if "$EMBLD" -e _start -Ttext 0x80100000 "$@" -o "$out/r.elf" \
           > "$out/r.txt" 2>&1; then
        echo "embld linked $what"; exit 1
    fi
    grep -q "$pat" "$out/r.txt" || {
        echo "embld refused $what, but not by name:"; cat "$out/r.txt"
        exit 1; }
}
printf '.text\n.globl f\nf: lw $2, %%gp_rel(x)($28)\n.data\n.globl x\nx: .word 1\n' \
    > "$out/gp.s"
mc "$out/gp.s" -o "$out/gp.o" || { echo "llvm-mc rejected gp.s"; exit 1; }
refuse "gp-relative small data" "R_MIPS_GPREL16" "$out/boot.o" "$out/io.o" \
    "$out/gp.o" "$out/far.o"
printf '.text\n.globl g\ng: jr $ra\nnop\n' > "$out/hard.s"
"$MC" -triple=$MT -mcpu=mips32r2 -filetype=obj \
    "$out/hard.s" -o "$out/hard.o" || { echo "llvm-mc rejected hard.s"; exit 1; }
refuse "a hard-float object with soft-float ones" "floating-point ABI" \
    "$out/boot.o" "$out/io.o" "$out/far.o" "$out/hard.o"
printf '.text\n.globl h\nh: lui $2, %%hi(x)\njr $ra\nnop\n.data\nx: .word 1\n' \
    > "$out/lone.s"
mc "$out/lone.s" -o "$out/lone.o" || { echo "llvm-mc rejected lone.s"; exit 1; }
refuse "an R_MIPS_HI16 with no R_MIPS_LO16" "no R_MIPS_LO16" \
    "$out/boot.o" "$out/io.o" "$out/far.o" "$out/lone.o"
# a GNU ld script: laid out for ARM and RISC-V only, so a MIPS image is
# refused rather than laid out by rules nobody checked for it
printf 'SECTIONS { .text 0x80100000 : { *(.text*) } .data : { *(.data*) } }\n' \
    > "$out/s.ld"
if "$EMBLD" -T "$out/s.ld" -e _start "$out/boot.o" "$out/io.o" "$out/far.o" \
       -o "$out/s.elf" > "$out/s.txt" 2>&1; then
    echo "embld linked a MIPS image by a linker script"; exit 1
fi
grep -q 'a linker script is supported for ARM, RISC-V and AVR images only' \
    "$out/s.txt" || {
    echo "embld refused a MIPS linker script, but not by name:"
    cat "$out/s.txt"; exit 1; }
echo "gp-relative data, a hard-float object, a lone HI16 and a linker script are refused by name"
