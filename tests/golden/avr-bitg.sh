#!/bin/sh
# GCC's type-generic bit builtins and C23's stdc_ ones on the ATmega328P,
# where int is 16 bits and long 32: tests/exec/builtin-bitg.c, a width
# per image (BITG_ONLY: all of it is four times the flash), at -O0,
# -O1, -O2 and -Os, and at -O2 and -Os with each allocation mode forced
# (EMBCC_AVR_RA_MODE: best-of-three only ever runs the winner), on
# qemu-system-avr's uno, where it must exit 42 as on every other board.
set -u
echo "TEST-MARKER avr-bitg"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/avr-bitg
rm -rf "${out:?}"; mkdir -p "$out"
EMBCC_AVR_HARNESS=$PWD/$out; export EMBCC_AVR_HARNESS

# the harness, and as its rt*.o (which link.sh links, every one): the
# 16- and 32-bit multiply and divide, and the 64-bit ones
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/io.o" || {
    echo "the harness does not compile"; exit 1; }
for f in lib/rt/avr.c lib/rt/avr64.c; do
    "$EMBCC" --target=avr -Os -c "$f" -o "$out/rt-$(basename "$f" .c).o" || {
        echo "$f does not compile"; exit 1; }
done
cat > "$out/drv.c" <<'EOT'
void puts_(const char *s);
void putn(long v);
int prog_main(void);
int main(void)
{
    int r = prog_main();
    if (r == 42) {
        puts_("bitg ok\n==END==\n");
    } else {
        puts_("bitg BAD ");
        putn(r);
        puts_("\n==END==\n");
    }
    for (;;)
        ;
}
EOT
"$EMBCC" --target=avr -Os -c "$out/drv.c" -o "$out/drv.o" || {
    echo "the driver does not compile"; exit 1; }

n=0; fail=0
run() {             # run WIDTH OPT [MODE]: builtin-bitg.c's width WIDTH
    tag=w$1$2${3:+-m$3}
    EMBCC_AVR_RA_MODE=${3:-} "$EMBCC" --target=avr $2 -Dmain=prog_main \
        -DBITG_ONLY=$1 -c tests/exec/builtin-bitg.c -o "$out/$tag.o" \
        2> "$out/$tag.err" || {
        echo "$tag: does not compile:"; head -3 "$out/$tag.err"; fail=1
        return; }
    sh tests/harness/avr/link.sh "$out/$tag.elf" "$out/$tag.o" \
        "$out/drv.o" > "$out/$tag.ln" 2>&1 || {
        echo "$tag: does not link:"; head -3 "$out/$tag.ln"; fail=1; return; }
    got=$(EMBCC_QEMU_UNTIL=END sh tests/harness/avr/run.sh "$out/$tag.elf" \
              2>/dev/null | grep bitg)
    if [ "$got" = "bitg ok" ]; then
        n=$((n + 1))
    else
        echo "FAIL $tag: ${got:-no output}"; fail=1
    fi
}
# one width per image (BITG_ONLY): char, int (16 bits), long (32), long
# long, and the result types and constants -- long long's at -O0 and -O1
# is more than the 32 KiB
for w in 0 1 2 3 4 6; do
    for o in -O0 -O1 -O2 -Os; do
        [ $w = 4 ] && [ $o = -O0 -o $o = -O1 ] && continue
        run $w $o
    done
    for m in 1 2 3; do
        run $w -O2 $m
        run $w -Os $m
    done
done
[ $fail = 0 ] || exit 1
echo "avr-bitg: $n runs of builtin-bitg.c, a width at a time, exit 42 on the ATmega328P"
