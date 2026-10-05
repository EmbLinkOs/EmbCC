#!/bin/sh
# AVR: a stack argument is stored without disturbing a register argument
# that is already in place.
#
# A call's eight-byte argument that goes on the stack was copied through
# r18 up -- and r18-r25 are where the first eight-byte argument goes, and
# where a 64-bit call result arrives. So in `stk(minus5(), 7, 9, big)` the
# result of minus5(), already in r18-r25, was overwritten by big's bytes
# before the call, at -O2 and -Os; at -O0, where `-5` is computed into
# its argument registers, the same. The bytes now go through r0 when the
# scratch would land on a live argument.
set -u
echo "TEST-MARKER avr-call-scratch"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/avr-call-scratch
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: no $QEMU"; exit 0; }
H=$out/h; mkdir -p "$H"
cat > "$out/p.c" <<'EOF'
void puts_(const char *s);
__attribute__((noinline)) static long long minus5(void) { return -5; }
__attribute__((noinline)) static long long stk(long long a, long b, long c,
                                               long long d)
{
    return a * 1000 + b * 100 + c * 10 + (d & 0xff);
}
__attribute__((noinline)) static long long stk2(long long a, long long b,
                                                long long d)
{
    return a + b * 3 + d;
}
int main(void)
{
    volatile long long k = -5;
    long long a = k;
    int bad = 0;
    bad |= stk(minus5(), 7L, 9L, 0x0123456789ABCDEFLL) !=
           -5000 + 700 + 90 + 0xef;
    bad |= stk(a, 7L, 9L, 0x0123456789ABCDEFLL) != -5000 + 700 + 90 + 0xef;
    bad |= stk2(minus5(), a, 0x1111111111111111LL) !=
           -5 - 15 + 0x1111111111111111LL;
    puts_(bad ? "BAD\n" : "ok\n");
    puts_("DONE\n");
    for (;;) ;
}
EOF
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" ||
    fail "the startup did not assemble"
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$H/io.o" || exit 1
    "$EMBCC" --target=avr $O -c lib/rt/avr.c -o "$H/rt.o" || exit 1
    "$EMBCC" --target=avr $O -c lib/rt/avr64.c -o "$H/rt64.o" || exit 1
    "$EMBCC" --target=avr $O -c "$out/p.c" -o "$out/p.o" || fail "$O: p.c"
    EMBCC_AVR_HARNESS="$H" sh tests/harness/avr/link.sh "$out/p.elf" \
        "$out/p.o" 2> "$out/l.err" || { cat "$out/l.err"; fail "$O: link"; }
    EMBCC_QEMU_UNTIL=DONE EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-30} \
        sh tests/harness/avr/run.sh "$out/p.elf" > "$out/run$O.txt" 2>/dev/null
    r=$(tr -d '\r' < "$out/run$O.txt" | head -1)
    [ "$r" = ok ] || { cat "$out/run$O.txt"; fail "$O: '$r'"; }
done
echo "avr-call-scratch: a stack argument leaves a register argument alone at -O0, -O1, -O2 and -Os"
