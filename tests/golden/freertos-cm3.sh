#!/bin/sh
# FreeRTOS, built by EmbCC from its own sources, runs on a Cortex-M3.
#
# The kernel (tasks.c, queue.c, list.c, timers.c, heap_4.c) and its GCC
# ARM_CM3 port are compiled unmodified, with the FreeRTOSConfig.h, startup
# and linker script in tests/golden/freertos-cm3/, and run on QEMU's
# lm3s6965evb. The port is where a compiler is tested: xPortPendSVHandler,
# vPortSVCHandler and prvPortStartFirstTask are naked functions whose asm
# saves and restores a task (an "i" operand, `bl vTaskSwitchContext`, a
# literal pool naming pxCurrentTCB), declared naked on the prototype and
# not on the definition; the critical sections are inline asm on BASEPRI.
#
# main.c runs a producer and a consumer through a queue, a mutex around
# the output, a one-shot software timer (the timer task), and a task that
# never blocks at the consumer's priority -- which only time slicing lets
# the others past. What it prints is fixed; any wrong save or restore of a
# register, or a lost tick, prints something else or nothing.
#
# The kernel is not in this repository. It is looked for in $EMBREF
# (default ~/EmbRef/FreeRTOS-Kernel, a clone of the FreeRTOS-Kernel
# repository); without it, or without qemu-system-arm, SKIP.
set -u
echo "TEST-MARKER freertos-cm3"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
K=${EMBREF:-$HOME/EmbRef}/FreeRTOS-Kernel
[ -f "$K/tasks.c" ] && [ -f "$K/portable/GCC/ARM_CM3/port.c" ] ||
    { echo "SKIP: no FreeRTOS-Kernel in ${EMBREF:-$HOME/EmbRef}"; exit 0; }
command -v "$QARM" >/dev/null 2>&1 || { echo "SKIP: no $QARM"; exit 0; }
d=tests/golden/freertos-cm3
out=tests/golden/out/freertos-cm3
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
T=--target=thumbv7m-none-eabi

# the compiler runtime: pdMS_TO_TICKS divides a 64-bit product
sh tools/build-rt.sh thumbv7m-none-eabi "$out/rt" > "$out/rt.log" 2>&1 ||
    { cat "$out/rt.log"; fail "librt for thumbv7m"; }

printf '%s\n' "FreeRTOS $(sed -n 's/^#define tskKERNEL_VERSION_NUMBER *"\(.*\)"/\1/p' \
    "$K/include/task.h") on EmbCC" 'got 1' 'got 2' 'got 3' 'got 4' 'got 5' \
    'sum 15' 'waited enough 1' 'timer 1' 'done' > "$out/want.txt"
# -O1 is not here: it keeps every value in a stack slot on this target,
# and the timer task's 512-byte stack overflows (which the kernel's own
# check reports). Not a miscompile, and a measure of what -O1 lacks.
for O in -O0 -O2 -Os; do
    o=$out/o${O#-O}; mkdir -p "$o"
    objs=
    for f in "$d/startup.c" "$d/main.c" "$K/tasks.c" "$K/queue.c" \
             "$K/list.c" "$K/timers.c" "$K/portable/MemMang/heap_4.c" \
             "$K/portable/GCC/ARM_CM3/port.c"; do
        b=$(basename "$f" .c)
        "$EMBCC" $T $O -I"$d" -I"$K/include" -I"$K/portable/GCC/ARM_CM3" \
            -c "$f" -o "$o/$b.o" 2> "$o/$b.err" ||
            { head -5 "$o/$b.err"; fail "$O: $b.c did not compile"; }
        objs="$objs $o/$b.o"
    done
    "$EMBCC" $T -nostdlib -T "$d/lm3s.ld" $objs "$out/rt/librt.a" \
        -o "$o/fw.elf" 2> "$o/link.err" ||
        { cat "$o/link.err"; fail "$O: the link"; }
    sh tests/harness/qrun.sh 20 --until done "$QARM" -M lm3s6965evb \
        -cpu cortex-m3 -nographic -kernel "$o/fw.elf" > "$o/run.txt" 2>&1
    tr -d '\r' < "$o/run.txt" | sed -n '1,/^done$/p' > "$o/got.txt"
    cmp -s "$o/got.txt" "$out/want.txt" || {
        echo "--- got"; cat "$o/run.txt"; echo "--- want"; cat "$out/want.txt"
        fail "$O: FreeRTOS did not run as written"; }
    echo "freertos-cm3 $O: three tasks, a queue, a mutex and a timer run"
done
