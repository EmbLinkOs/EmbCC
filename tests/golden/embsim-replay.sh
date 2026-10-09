#!/bin/sh
# embsim --input, --record and --replay: input the run cannot know in
# advance, recorded, and the run again from the record.
#
# tests/golden/embsim-an/rr.c takes input on each core: the M3's PL011 by
# its receive interrupt with SysTick ticking (mode 1), and with nothing
# else to wake it (mode 5, the run waits for the host); semihosting's
# SYS_READC (mode 2); the M4's CMSDK UART, polled (mode 3); a debugger's
# register and memory writes (mode 4); RV32's NS16550A, polled; the
# AVR's USART0 by its interrupt, with Timer/Counter1 ticking. The input
# comes through a pipe, a few bytes at a time with pauses, so when each
# arrives (and the ticks the program prints with it) depends on the host.
# For each:
#   - the record has the bytes, and the interrupts they raised;
#   - the replay, with other bytes or none on stdin, prints what the
#     recorded run printed, to the byte, ends the same way, with the same
#     --count, and says the run is the recording's;
# and a record changed -- a byte, an event's step -- or replayed on
# another image is refused, with where the runs went apart.
set -u
echo "TEST-MARKER embsim-replay"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
GDB=${EMBCC_GDB:-gdb}
export EMBLD
out=tests/golden/out/embsim-replay
rm -rf "${out:?}"; mkdir -p "$out"
src=tests/golden/embsim-an/rr.c
fail=0
[ -x "$EMBSIM" ] || { echo "FAIL: $EMBSIM is not built (make embsim)"; exit 1; }
simpids=
cleanup() { for p in $simpids; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

# the harnesses
for a in m3 rv32 avr; do mkdir -p "$out/$a"; done
"$EMBCC" --target=thumbv7m-none-eabi -O0 -c tests/harness/thumb/boot.c -o "$out/m3/boot.o" &&
"$EMBCC" --target=thumbv7m-none-eabi -O0 -c tests/harness/thumb/io.c -o "$out/m3/io.o" &&
"$EMBCC" --target=riscv32-unknown-elf -O0 -c tests/harness/riscv/boot.c -o "$out/rv32/boot.o" &&
"$EMBCC" --target=riscv32-unknown-elf -O0 -c tests/harness/riscv/io.c -o "$out/rv32/io.o" &&
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/avr/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/avr/io.o" &&
sh tools/build-rt.sh avr "$out/avr" > "$out/avr/rt.log" 2>&1 ||
    { echo "FAIL: the harnesses do not build"; exit 1; }

# build TAG ARCH MODE [FLAGS]: rr.c as $out/TAG.elf
build() {
    tag=$1; arch=$2; mode=$3; shift 3
    case $arch in
    m3) t=thumbv7m-none-eabi; H=EMBCC_THUMB_HARNESS; l=thumb ;;
    m4) t=thumbv7em-none-eabi; H=EMBCC_THUMB_HARNESS; l=thumb; arch=m3 ;;
    rv32) t=riscv32-unknown-elf; H=EMBCC_RISCV_HARNESS; l=riscv ;;
    avr) t=avr; H=EMBCC_AVR_HARNESS; l=avr ;;
    esac
    "$EMBCC" --target=$t -O1 "$@" -DMODE="$mode" -c "$src" -o "$out/$tag.o" &&
    env "$H=$out/$arch" sh tests/harness/$l/link.sh "$out/$tag.elf" "$out/$tag.o" > /dev/null 2>&1 ||
        { echo "FAIL $tag: rr.c does not build"; fail=1; return 1; }
}

# feed: the input, a few bytes at a time, with the host's pauses between
feed() {
    for chunk in "$@"; do
        printf '%s' "$chunk"
        sleep 0.2
    done
}

# rr TAG ARGS... -- CHUNK...: record TAG fed CHUNKs, replay it with other
# bytes on stdin, and compare
rr() {
    tag=$1; shift
    args=
    while [ "$1" != -- ]; do args="$args $1"; shift; done
    shift
    # shellcheck disable=SC2086
    feed "$@" | timeout 60 "$EMBSIM" "$out/$tag.elf" $args --max-insns 300000000 \
        --input - --record "$out/$tag.rec" --count "$out/$tag.c1" --stats \
        > "$out/$tag.o1" 2> "$out/$tag.e1"
    s1=$?
    printf 'zzzzzzzz' | timeout 60 "$EMBSIM" --replay "$out/$tag.rec" --count "$out/$tag.c2" \
        --stats > "$out/$tag.o2" 2> "$out/$tag.e2"
    s2=$?
    [ $s1 = $s2 ] && cmp -s "$out/$tag.o1" "$out/$tag.o2" && cmp -s "$out/$tag.c1" "$out/$tag.c2" &&
    grep -q '^embsim: replay: the run is the recording' "$out/$tag.e2" &&
    [ "$(grep -v '^embsim: replay' "$out/$tag.e2")" = "$(cat "$out/$tag.e1")" ] || {
        echo "FAIL $tag: the replay is not the recorded run (status $s1, $s2):"
        diff "$out/$tag.o1" "$out/$tag.o2" | head -5 | sed 's/^/     | /'
        sed 's/^/     | /' "$out/$tag.e2" | head -5
        fail=1; return 1; }
    for chunk in "$@"; do
        for c in $(printf '%s' "$chunk" | od -An -tx1); do
            grep -q " rx $c\$" "$out/$tag.rec" || {
                echo "FAIL $tag: the record has no rx $c"; fail=1; }
        done
    done
}

# ---- the inputs, recorded and replayed -------------------------------------
if build m3-1 m3 1 && rr m3-1 --board lm3s6965evb -- ab cd q; then
    grep -q '^@[0-9]* [0-9]* exc 21$' "$out/m3-1.rec" &&
    grep -q '^@[0-9]* [0-9]* exc 15 x [0-9]* +[0-9]* +[0-9]*$' "$out/m3-1.rec" &&
    [ "$(grep -c '^got ' "$out/m3-1.o1")" = 5 ] &&
    echo "  M3, PL011 interrupt with SysTick: $(tr '\n' ' ' < "$out/m3-1.o1" | sed 's/  */ /g')replayed" ||
    { echo "FAIL m3-1: the record lacks the UART's interrupts (21) or SysTick's runs (15), or the output"; fail=1; }
fi
if build m3-5 m3 5 && rr m3-5 --board lm3s6965evb -- a b q; then
    [ "$(grep -c ' wake$' "$out/m3-5.rec")" = 3 ] &&
    echo "  M3, PL011 interrupt alone: the run waited for the host 3 times, replayed" ||
    { echo "FAIL m3-5: the record has $(grep -c ' wake$' "$out/m3-5.rec") wakes, not 3"; fail=1; }
fi
if build m4-3 m4 3 && rr m4-3 --board mps2-an386 -- hi q; then
    grep -q '^hiq$' "$out/m4-3.o1" && echo "  M4, CMSDK UART polled: echoed, replayed" ||
    { echo "FAIL m4-3: the echo"; fail=1; }
fi
if build rv32 rv32 1 && rr rv32 --board virt --ram-size 8M -- xy q; then
    [ "$(grep -c '^got ' "$out/rv32.o1")" = 3 ] && echo "  RV32, NS16550A polled: replayed" ||
    { echo "FAIL rv32: the output"; fail=1; }
fi
if build avr avr 1 && rr avr --board uno -- xy q; then
    [ "$(grep -c '^got ' "$out/avr.o1")" = 3 ] && grep -q ' exc 18$' "$out/avr.rec" &&
    echo "  AVR, USART0's receive interrupt with Timer/Counter1: replayed" ||
    { echo "FAIL avr: the output, or no receive interrupt (18) in the record"; fail=1; }
fi

# SYS_READC: the record's bytes, whatever stdin has now
f0=$fail
build m3-2 m3 2 &&
printf 'xyz' | "$EMBSIM" "$out/m3-2.elf" --record "$out/m3-2.rec" --max-insns 1000000 > "$out/m3-2.o1" 2>&1 &&
printf 'abc' | "$EMBSIM" --replay "$out/m3-2.rec" > "$out/m3-2.o2" 2> "$out/m3-2.e2" &&
cmp -s "$out/m3-2.o1" "$out/m3-2.o2" && grep -q '^read 120 $' "$out/m3-2.o2" ||
    { echo "FAIL m3-2: SYS_READC's bytes are not replayed"; fail=1; }
[ $fail = "$f0" ] && echo "  M3, SYS_READC: 'xyz' replayed with 'abc' on stdin"

# ---- a record that is not the run's --------------------------------------
f0=$fail
sed 's/ rx 62$/ rx 42/' "$out/m3-1.rec" > "$out/bad-byte.rec"
"$EMBSIM" --replay "$out/bad-byte.rec" > "$out/bad-byte.out" 2>&1; st=$?
[ $st = 5 ] && grep -q '^embsim: replay diverged: the run ended at' "$out/bad-byte.out" ||
    { echo "FAIL: a changed byte (status $st)"; tail -3 "$out/bad-byte.out"; fail=1; }
awk '!done && / rx 61$/ { $1 = "@" (substr($1, 2) + 1); done = 1 } { print }' "$out/m3-1.rec" > "$out/bad-step.rec"
"$EMBSIM" --replay "$out/bad-step.rec" > "$out/bad-step.out" 2>&1; st=$?
[ $st = 5 ] && grep -q '^embsim: replay diverged at step [0-9]* ([0-9]* instructions): ' "$out/bad-step.out" ||
    { echo "FAIL: a byte moved by a step (status $st)"; tail -3 "$out/bad-step.out"; fail=1; }
build other m3 5 &&
"$EMBSIM" "$out/other.elf" --replay "$out/m3-1.rec" > "$out/other.out" 2>&1; st=$?
[ $st = 2 ] && grep -q 'is not the image the recording ran' "$out/other.out" ||
    { echo "FAIL: another image (status $st)"; cat "$out/other.out"; fail=1; }
"$EMBSIM" "$out/m3-1.elf" --board mps2-an386 --replay "$out/m3-1.rec" > "$out/board.out" 2>&1; st=$?
[ $st = 2 ] && grep -q 'the recording ran on lm3s6965evb' "$out/board.out" ||
    { echo "FAIL: another board (status $st)"; fail=1; }
[ $fail = "$f0" ] && echo "  a changed byte, a byte a step late, another image and another board: refused"

# ---- a debugger's writes ------------------------------------------------------
# and a read watchpoint, which stops the run before the read: a step that
# does nothing, which the replay without gdb never takes
if command -v "$GDB" >/dev/null 2>&1 &&
   "$GDB" -nx -batch -ex 'set architecture arm' 2>&1 | grep -q 'architecture is set to "arm"'; then
    f0=$fail
    build m3-4 m3 4 -g
    port=$(( 35000 + ($$ % 1000) * 3 ))
    "$EMBSIM" "$out/m3-4.elf" --gdb $port --gdb-wait --record "$out/m3-4.rec" \
        --count "$out/m3-4.c1" --max-insns 1000000 > "$out/m3-4.o1" 2> "$out/m3-4.e1" &
    simpids="$simpids $!"
    sleep 0.5
    timeout 60 "$GDB" -nx -batch -ex "file $out/m3-4.elf" -ex "target remote localhost:$port" \
        -ex 'break add1' -ex continue -ex 'set var bonus = 41' -ex 'set $r0 = 100' \
        -ex 'rwatch bonus' -ex continue -ex continue > "$out/m3-4.gdb" 2>&1
    wait
    "$EMBSIM" --replay "$out/m3-4.rec" --count "$out/m3-4.c2" > "$out/m3-4.o2" 2> "$out/m3-4.e2"
    s2=$?
    "$EMBSIM" "$out/m3-4.elf" --max-insns 1000000 > "$out/m3-4.plain" 2>&1
    [ $s2 = 0 ] && grep -q '^embsim: replay: the run is the recording' "$out/m3-4.e2" &&
    grep -q '^result 142 $' "$out/m3-4.o1" && cmp -s "$out/m3-4.o1" "$out/m3-4.o2" &&
    cmp -s "$out/m3-4.c1" "$out/m3-4.c2" && grep -q '^result 2 $' "$out/m3-4.plain" &&
    grep -q 'Value = 41' "$out/m3-4.gdb" &&
    grep -q ' mem 20000[0-9a-f]* 29000000$' "$out/m3-4.rec" && grep -q ' reg 0 64000000$' "$out/m3-4.rec" ||
        { echo "FAIL m3-4: gdb's writes (bonus = 41, r0 = 100) are not replayed:"
          cat "$out/m3-4.o1" "$out/m3-4.o2" "$out/m3-4.e2" | sed 's/^/     | /'; fail=1; }
    [ $fail = "$f0" ] && echo "  M3 under gdb: its memory and register writes replayed without it (result 142, 2 without them)"
else
    echo "  SKIP the debugger's writes: no gdb with ARM"
fi

"$EMBSIM" "$out/m3-1.elf" --replay "$out/m3-1.rec" --input - < /dev/null > "$out/bad.out" 2>&1; st=$?
[ $st = 2 ] && grep -q 'takes the run.s inputs from the recording' "$out/bad.out" ||
    { echo "FAIL: --replay with --input is not refused"; fail=1; }
[ $fail = 0 ] && echo "embsim --record/--replay: runs that took input replayed to the byte and the instruction, on every core; other records refused"
exit $fail
