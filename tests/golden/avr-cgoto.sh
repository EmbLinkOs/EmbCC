#!/bin/sh
# GNU computed goto on the ATmega328P: tests/exec/computed-goto.c and
# computed-goto-more.c, compiled for AVR at -O0, -O1, -O2 and -Os -- and
# at -O2 and -Os with each allocation mode forced (EMBCC_AVR_RA_MODE:
# best-of-three only ever runs the winner) -- and run on qemu-system-avr's
# uno, where each must exit 42 as it does on every other target. No other
# test runs the exec corpus on this part.
#
# A label address is a code address, and program memory is addressed in
# WORDS: &&label is the function's own symbol through the _GS forms, as a
# function pointer is (ldi lo8(gs(f+L)), ldi hi8(gs(f+L)), the label's
# byte offset L the addend, which the linker halves with the rest), and
# `goto *p` is ijmp through Z. The relocations are checked too: a data
# (byte) address there would be a jump twice as far in -- into the middle
# of something else, which still decodes. (avr-gcc spells it the same
# way; clang's AVR backend writes `ldi r18, .Ltmp0+2`, which is not an
# address at all, so it is no referee here.)
set -u
echo "TEST-MARKER avr-cgoto"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/avr-cgoto
rm -rf "${out:?}"; mkdir -p "$out"
EMBCC_AVR_HARNESS=$PWD/$out; export EMBCC_AVR_HARNESS

"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/io.o" &&
"$EMBCC" --target=avr -Os -c lib/rt/avr.c -o "$out/rt.o" || {
    echo "the harness does not compile"; exit 1; }
cat > "$out/drv.c" <<'EOF'
void puts_(const char *s);
int prog_main(void);
int main(void)
{
    puts_(prog_main() == 42 ? "cgoto ok\n==END==\n" : "cgoto BAD\n==END==\n");
    for (;;)
        ;
}
EOF
"$EMBCC" --target=avr -Os -c "$out/drv.c" -o "$out/drv.o" || {
    echo "the driver does not compile"; exit 1; }

n=0
run() {             # run SRC OPT [MODE]
    tag=$(basename "$1" .c)$2${3:+-m$3}
    EMBCC_AVR_RA_MODE=${3:-} "$EMBCC" --target=avr $2 -Dmain=prog_main \
        -c "$1" -o "$out/$tag.o" 2> "$out/$tag.err" || {
        echo "$tag: does not compile:"; head -3 "$out/$tag.err"; exit 1; }
    sh tests/harness/avr/link.sh "$out/$tag.elf" "$out/$tag.o" \
        "$out/drv.o" > "$out/$tag.ln" 2>&1 || {
        echo "$tag: does not link:"; head -3 "$out/$tag.ln"; exit 1; }
    got=$(EMBCC_QEMU_UNTIL=END sh tests/harness/avr/run.sh "$out/$tag.elf" \
              2>/dev/null | grep cgoto)
    [ "$got" = "cgoto ok" ] || { echo "$tag: ${got:-no output}"; exit 1; }
    n=$((n + 1))
}
for c in tests/exec/computed-goto.c tests/exec/computed-goto-more.c; do
    for o in -O0 -O1 -O2 -Os; do
        run "$c" $o
    done
    for m in 1 2 3; do
        run "$c" -O2 $m
        run "$c" -Os $m
    done
done

# every label address is a word address against a function's own symbol
"$RE" -r "$out/computed-goto-O2.o" > "$out/rel.txt"
lo=$(grep -c 'R_AVR_LO8_LDI_GS .* run + [1-9a-f]' "$out/rel.txt")
hi=$(grep -c 'R_AVR_HI8_LDI_GS .* run + [1-9a-f]' "$out/rel.txt")
[ "$lo" = 3 ] && [ "$hi" = 3 ] || {
    echo "run()'s three label addresses are not three lo8/hi8(gs(run+L)) pairs:"
    cat "$out/rel.txt"; exit 1; }
! grep -q 'R_AVR_\(LO8\|HI8\)_LDI ' "$out/rel.txt" || {
    echo "a label address took a data (byte) address:"; cat "$out/rel.txt"
    exit 1; }
echo "avr-cgoto: $n runs of the computed-goto programs exit 42 on the"
echo "ATmega328P, every label address a word address (gs) against its function"
