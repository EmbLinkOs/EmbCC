#!/bin/sh
# embsim: a loop that jumps to itself ends the run as soon as nothing can
# interrupt it, however EmbSim works out what can.
#
# The cores ask whether a device will raise an interrupt on every turn of
# such a loop, and EmbSim keeps the answer until something can change it:
# a device read or written, a clock started or stopped, a RISC-V CSR
# written, a reset, or the time of the event it expects. Each program of
# tests/golden/embsim-an/idle.c takes a timer's ticks, then takes away
# the last thing that could interrupt the loop -- SysTick switched off by
# a write; a CSR write to mie that touches no device (RV32); TIMSK1
# cleared (AVR); a one-pulse timer stopping itself with no write at all
# (the STM32F405's TIM2) -- and enters the loop. In the --trace, the loop
# must run exactly once after the last instruction that is not the loop,
# and the run must end there as an idle loop; a stale answer would run it
# until the expected event's time.
#
# What a program does to itself is followed by an event of the timer that
# was expected anyway. A debugger is not: gdb stops the M3 and RV32 in a
# loop whose timer's next tick is far off, takes the interrupt away --
# TICKINT cleared in SysTick's CSR, written in memory, with the counter
# left running; MTIE in mie, as a register -- and resumes. The
# loop must turn once more and wait, as it would without the answer kept
# (`monitor insns` one more), not spin on until the tick's time.
set -u
echo "TEST-MARKER embsim-idle"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
export EMBLD
out=tests/golden/out/embsim-idle
rm -rf "${out:?}"; mkdir -p "$out"
src=tests/golden/embsim-an/idle.c
fail=0
[ -x "$EMBSIM" ] || { echo "FAIL: $EMBSIM is not built (make embsim)"; exit 1; }

# check TAG WHAT ARGS...: the run ends idle at the loop, after one turn
check() {
    tag=$1; what=$2; shift 2
    "$EMBSIM" "$out/$tag.elf" "$@" --max-insns 2000000 --stats --trace "$out/$tag.trace" \
        > "$out/$tag.out" 2> "$out/$tag.err"
    st=$?
    loop=$(sed -n 's/.*a loop at 0x\([0-9a-f]*\) that nothing can interrupt.*/\1/p' "$out/$tag.err")
    if [ $st != 0 ] || [ -z "$loop" ]; then
        echo "FAIL $tag: the run did not end in an idle loop (status $st):"
        sed 's/^/     | /' "$out/$tag.err"; fail=1; return
    fi
    turns=$(awk -v l="$loop" '{ a = substr($1, 1, length($1) - 1); sub(/^0*/, "", a)
                              ll = l; sub(/^0*/, "", ll)
                              if (a == ll) n++; else n = 0 } END { print n + 0 }' "$out/$tag.trace")
    if [ "$turns" != 1 ]; then
        echo "FAIL $tag: the loop at 0x$loop ran $turns times after $what, not once"
        fail=1; return
    fi
    echo "  $tag: $what, then one turn of the loop at 0x$loop"
}

h=$out/m3; mkdir -p "$h"
"$EMBCC" --target=thumbv7m-none-eabi -O0 -c tests/harness/thumb/boot.c -o "$h/boot.o" &&
"$EMBCC" --target=thumbv7m-none-eabi -O0 -c tests/harness/thumb/io.c -o "$h/io.o" &&
"$EMBCC" --target=thumbv7m-none-eabi -O1 -DMODE=1 -c "$src" -o "$out/m3.o" &&
EMBCC_THUMB_HARNESS=$h sh tests/harness/thumb/link.sh "$out/m3.elf" "$out/m3.o" > /dev/null 2>&1 &&
check m3 "SysTick switched off" --board lm3s6965evb || true

h=$out/rv32; mkdir -p "$h"
"$EMBCC" --target=riscv32-unknown-elf -O0 -c tests/harness/riscv/boot.c -o "$h/boot.o" &&
"$EMBCC" --target=riscv32-unknown-elf -O0 -c tests/harness/riscv/io.c -o "$h/io.o" &&
"$EMBCC" --target=riscv32-unknown-elf -O1 -c "$src" -o "$out/rv32.o" &&
EMBCC_RISCV_HARNESS=$h sh tests/harness/riscv/link.sh "$out/rv32.elf" "$out/rv32.o" > /dev/null 2>&1 &&
check rv32 "MTIE cleared in mie" --board virt --ram-size 8M || true

h=$out/avr; mkdir -p "$h"
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$h/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$h/io.o" &&
sh tools/build-rt.sh avr "$h" > "$h/rt.log" 2>&1 &&
"$EMBCC" --target=avr -O1 -c "$src" -o "$out/avr.o" &&
EMBCC_AVR_HARNESS=$h sh tests/harness/avr/link.sh "$out/avr.elf" "$out/avr.o" > /dev/null 2>&1 &&
check avr "OCIE1A cleared" --board uno || true

svd=${EMBREF:-$HOME/EmbRef}/svd/STM32F405.svd
if [ -f "$svd" ]; then
    "$EMBCC" --target=thumbv7em-none-eabi -O1 -DMODE=2 -c "$src" -o "$out/f405.o" &&
    "$EMBLD" -e reset -Ttext 0x08000000 -Tdata 0x20000000 "$out/f405.o" -o "$out/f405.elf" > /dev/null 2>&1 &&
    check f405 "TIM2's one pulse and its handler" --board stm32f405 --svd "$svd" || true
else
    echo "  SKIP the STM32F405: no $svd"
fi
# ---- a debugger takes the interrupt away ------------------------------------
GDB=${EMBCC_GDB:-gdb}
gdb_idle() {    # TAG ARCH WRITE ELF ARGS...
    tag=$1; arch=$2; write=$3; elf=$4; shift 4
    port=$(( 37000 + ($$ % 500) * 4 + ${#tag} ))
    "$EMBSIM" "$elf" "$@" --gdb $port --max-insns 3000000000 > "$out/$tag.sim" 2>&1 &
    sp=$!
    sleep 0.5
    printf 'set pagination off\nset confirm off\ncontinue\nmonitor insns\n%s\ncontinue\nmonitor insns\nkill\n' \
        "$write" > "$out/$tag.gdbx"
    "$GDB" -nx -batch -ex "set architecture $arch" -ex "file $elf" \
        -ex "target remote localhost:$port" -x "$out/$tag.gdbx" > "$out/$tag.gdb" 2>&1 &
    gp=$!
    for k in 1 2; do
        sleep 2
        kill -INT $gp 2>/dev/null
    done
    i=0
    while kill -0 $gp 2>/dev/null && [ $i -lt 100 ]; do sleep 0.2; i=$((i + 1)); done
    kill -9 $gp $sp 2>/dev/null
    wait $sp 2>/dev/null
    set -- $(grep -E '^[0-9]+$' "$out/$tag.gdb")
    if [ $# != 2 ]; then
        echo "FAIL $tag: the session did not stop twice:"; sed 's/^/     | /' "$out/$tag.gdb" | head -12
        fail=1; return
    fi
    if [ $(($2 - $1)) -gt 2 ]; then
        echo "FAIL $tag: after the interrupt was taken away the loop ran $(($2 - $1)) more instructions, not one"
        fail=1; return
    fi
    echo "  $tag: gdb took the interrupt away ($write); the loop turned $(($2 - $1)) more time and waited"
}
if command -v "$GDB" >/dev/null 2>&1 &&
   "$GDB" -nx -batch -ex 'set architecture arm' 2>&1 | grep -q 'architecture is set to "arm"'; then
    "$EMBCC" --target=thumbv7m-none-eabi -O1 -DMODE=3 -c "$src" -o "$out/m3-gdb.o" &&
    EMBCC_THUMB_HARNESS=$out/m3 sh tests/harness/thumb/link.sh "$out/m3-gdb.elf" "$out/m3-gdb.o" > /dev/null 2>&1 &&
    gdb_idle m3-gdb arm 'set *(unsigned *)0xE000E010 = 1' "$out/m3-gdb.elf" --board lm3s6965evb
    if "$GDB" -nx -batch -ex 'set architecture riscv:rv32' 2>&1 | grep -q 'riscv:rv32'; then
        "$EMBCC" --target=riscv32-unknown-elf -O1 -DMODE=3 -c "$src" -o "$out/rv32-gdb.o" &&
        EMBCC_RISCV_HARNESS=$out/rv32 sh tests/harness/riscv/link.sh "$out/rv32-gdb.elf" "$out/rv32-gdb.o" > /dev/null 2>&1 &&
        gdb_idle rv32-gdb riscv:rv32 'set $mie = 8' "$out/rv32-gdb.elf" --board virt --ram-size 8M
    fi
else
    echo "  SKIP the debugger: no gdb with ARM"
fi

for t in m3 rv32 avr; do
    [ -f "$out/$t.elf" ] || { echo "FAIL $t: idle.c does not build"; fail=1; }
done
[ $fail = 0 ] && echo "embsim: idle loops end at their first turn once nothing can interrupt them"
exit $fail
