#!/bin/sh
# setjmp and longjmp on the ATmega328P: lib/avr/setjmp.c, with
# tests/exec/setjmp-registers.c (every callee-saved register, r2-r17 and
# Y, comes back), setjmp-longjmp.c and volatile-local-longjmp.c, compiled
# for AVR at -O0, -O1, -O2 and -Os -- and at -O2 and -Os with each
# allocation mode forced (EMBCC_AVR_RA_MODE: best-of-three only ever runs
# the winner) -- and run on qemu-system-avr's uno, where each must exit
# 42 as it does on every other target. The exec goldens of the other
# boards run these with lib/libc; the AVR's library is lib/avr's.
set -u
echo "TEST-MARKER avr-setjmp"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/avr-setjmp
rm -rf "${out:?}"; mkdir -p "$out"
EMBCC_AVR_HARNESS=$PWD/$out; export EMBCC_AVR_HARNESS

# the harness, and as its rt*.o (which link.sh links, every one): the
# multiply and divide, the binary32 the floats in setjmp-longjmp.c call,
# and the setjmp under test
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/io.o" || {
    echo "the harness does not compile"; exit 1; }
for f in lib/rt/avr.c lib/rt/avrfp.c lib/rt/avrfpadd.c lib/rt/avrfpmul.c \
         lib/rt/avrfpdiv.c lib/rt/avrfpcmp.c lib/rt/avrfpi.c lib/avr/setjmp.c; do
    "$EMBCC" --target=avr -Os -c "$f" -o "$out/rt-$(basename "$f" .c).o" || {
        echo "$f does not compile"; exit 1; }
done
cat > "$out/drv.c" <<'EOF'
void puts_(const char *s);
void putn(long v);
int prog_main(void);
int main(void)
{
    int r = prog_main();
    if (r == 42) {
        puts_("setjmp ok\n==END==\n");
    } else {
        puts_("setjmp BAD ");
        putn(r);
        puts_("\n==END==\n");
    }
    for (;;)
        ;
}
EOF
"$EMBCC" --target=avr -Os -c "$out/drv.c" -o "$out/drv.o" || {
    echo "the driver does not compile"; exit 1; }

n=0; fail=0
run() {             # run SRC OPT [MODE]
    tag=$(basename "$1" .c)$2${3:+-m$3}
    EMBCC_AVR_RA_MODE=${3:-} "$EMBCC" --target=avr $2 -Dmain=prog_main \
        -c "$1" -o "$out/$tag.o" 2> "$out/$tag.err" || {
        echo "$tag: does not compile:"; head -3 "$out/$tag.err"; fail=1
        return; }
    sh tests/harness/avr/link.sh "$out/$tag.elf" "$out/$tag.o" \
        "$out/drv.o" > "$out/$tag.ln" 2>&1 || {
        echo "$tag: does not link:"; head -3 "$out/$tag.ln"; fail=1; return; }
    got=$(EMBCC_QEMU_UNTIL=END sh tests/harness/avr/run.sh "$out/$tag.elf" \
              2>/dev/null | grep setjmp)
    if [ "$got" = "setjmp ok" ]; then
        n=$((n + 1))
    else
        echo "FAIL $tag: ${got:-no output}"; fail=1
    fi
}
for c in tests/exec/setjmp-registers.c tests/exec/setjmp-longjmp.c \
         tests/exec/volatile-local-longjmp.c; do
    for o in -O0 -O1 -O2 -Os; do
        run "$c" $o
    done
    for m in 1 2 3; do
        run "$c" -O2 $m
        run "$c" -Os $m
    done
done
[ $fail = 0 ] || exit 1
echo "avr-setjmp: $n runs of the setjmp programs exit 42 on the ATmega328P"
