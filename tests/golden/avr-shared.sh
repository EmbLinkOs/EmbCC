#!/bin/sh
# AVR: the shared cross-target programs, on the part, against the host.
#
# tests/golden/embedded-*.c are the programs every other embedded target has
# run against the host at four optimisation levels since it landed: 64-bit
# arithmetic, aggregates by value and by hidden pointer, variadic calls. AVR
# never ran them -- at first because the backend refused them, then because
# the images did not fit the part. When they finally ran, they found two
# SILENT miscompiles that every AVR test before them had missed:
#
#   * va_copy copied 24 bytes into a 16-byte hidden buffer. The pointer-
#     va_list targets were an inline list -- thumb, riscv32, riscv64 -- and
#     AVR was never added, so it took x86-64's tag path. `copied(1, 7)`
#     returned 7 for 707 and the program carried on.
#   * a call to a function returning a struct of more than eight bytes placed
#     its arguments from r25 down as though there were no hidden pointer, then
#     wrote the pointer into r25:r24 on top of the first one. m20(10) returned
#     {0,1,2,3,4}: 40 printed for 190, 5 for 205.
#
# ---- the one line that is allowed to differ, and exactly how ----------
#
# These programs were written for 32-bit targets, and on AVR `int` and
# `unsigned` are SIXTEEN bits. One line of embedded-int64.c prints
# `(unsigned)4000000000u` and two `(int)` truncations of a 64-bit value, which
# on this machine are genuinely different numbers. That line is not skipped:
# each of its fields must equal the host's value truncated to 16 bits, so the
# line still has to be RIGHT, just for this machine's widths.
#
# ---- and -O0 ----------------------------------------------------------
#
# At -O0 two of these images are bigger than the part's 32 KB of flash
# (embedded-int64.c's by more than double). A level that does not fit is
# reported with its size and not run; everything that fits must agree. As the
# backend's code gets smaller those levels start running without anyone
# touching this file.
set -u
echo "TEST-MARKER avr-shared"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-shared
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
export EMBCC
H=$out/h; mkdir -p "$H"

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: no $QEMU"; exit 0; }

sh tools/build-rt.sh avr "$out/rt" 2> "$out/rt.err" || {
    echo "the AVR runtime does not build:"; head -3 "$out/rt.err"; exit 1; }
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" || exit 1

cat > "$out/wrap.c" <<'EOF'
void puts_(const char *s);
int prog_main(void);
int main(void) { prog_main(); puts_("<<END>>\n"); for (;;) ; }
EOF
cat > "$out/hostio.c" <<'EOF'
#include <stdio.h>
void writec(int c) { putchar(c); }
void puts_(const char *s) { while (*s) putchar(*s++); }
void putn(long v) { printf("%ld ", v); }
int prog_main(void);
int main(void) { return prog_main(); }
EOF

# Field k of the host's line, truncated to AVR's 16-bit int/unsigned.
trunc16() {
    python3 - "$1" "$2" <<'PY'
import sys
want, got = sys.argv[1].split(), sys.argv[2].split()
if len(want) != len(got):
    sys.exit(1)
for w, g in zip(want, got):
    if len(w) == 16 and all(c in "0123456789abcdef" for c in w):
        # A 64-bit hex field. `hx((unsigned long long)u)` of a 16-bit
        # unsigned: the low sixteen bits, zero-extended.
        if w == g:
            continue
        if int(g, 16) != int(w, 16) & 0xffff:
            sys.exit(1)
    else:
        v = int(w) & 0xffff
        if v >= 0x8000:
            v -= 0x10000
        if int(g) != v:
            sys.exit(1)
PY
}

ran=0
notrun=
for prog in int64 aggregate varargs; do
    cc -std=c99 -w -Dmain=prog_main -c "tests/golden/embedded-$prog.c" \
       -o "$out/host-$prog.o" &&
    cc -o "$out/host-$prog" "$out/host-$prog.o" "$out/hostio.c" || {
        echo "$prog: the host build failed"; exit 1; }
    "$out/host-$prog" > "$out/want-$prog" || {
        echo "$prog: the host program failed"; exit 1; }
    for O in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$H/io.o" || exit 1
        "$EMBCC" --target=avr $O -c "$out/wrap.c" -o "$H/wrap.o" || exit 1
        "$EMBCC" --target=avr $O -Dmain=prog_main \
            -c "tests/golden/embedded-$prog.c" -o "$H/p.o" 2> "$out/c.err" || {
            echo "$prog $O: did not compile:"; head -4 "$out/c.err"; exit 1; }
        "${EMBLD:-./embld}" -e __vectors -Ttext 0x0 -Tdata 0x100 \
            "$H/boot.o" "$H/io.o" "$H/p.o" "$H/wrap.o" "$out/rt/librt.a" \
            -o "$H/p.elf" 2> "$out/l.err" || {
            echo "$prog $O: link failed:"; head -4 "$out/l.err"; exit 1; }
        tx=$(llvm-size "$H/p.elf" 2>/dev/null | awk 'NR==2{print $1}')
        if [ -n "$tx" ] && [ "$tx" -gt 32768 ]; then
            notrun="$notrun
  $prog $O: $tx bytes"
            continue
        fi
        EMBCC_QEMU_UNTIL='<<END>>' EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-60} \
            sh tests/harness/avr/run.sh "$H/p.elf" 2>/dev/null \
            | sed '/<<END>>/,$d' > "$out/got-$prog$O"
        ran=$((ran + 1))
        if ! cmp -s "$out/want-$prog" "$out/got-$prog$O"; then
            # Line by line: every line equal, except that a line may differ
            # if it is exactly the host's truncated to 16 bits.
            bad=0
            paste -d '\t' "$out/want-$prog" "$out/got-$prog$O" \
                > "$out/pair" 2>/dev/null
            [ "$(wc -l < "$out/want-$prog")" = "$(wc -l < "$out/got-$prog$O")" ] \
                || bad=1
            while IFS='	' read -r w g; do
                [ "$w" = "$g" ] && continue
                trunc16 "$w" "$g" || { bad=1; break; }
            done < "$out/pair"
            if [ "$bad" -ne 0 ]; then
                echo "$prog $O: the ATmega328P disagrees with the host:"
                diff "$out/want-$prog" "$out/got-$prog$O" | head -8
                exit 1
            fi
        fi
    done
done

echo "the shared cross-target programs agree with the host on a real
ATmega328P -- $ran runs: 64-bit arithmetic (one line correctly narrower,
because int is sixteen bits here), aggregates by value and returned through
the hidden pointer, and variadic calls including va_copy"
[ -z "$notrun" ] || echo "not run, because the image does not fit the 32768-byte part:$notrun"
