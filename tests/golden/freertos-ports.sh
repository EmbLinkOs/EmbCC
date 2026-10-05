#!/bin/sh
# FreeRTOS's Cortex-M4F and Cortex-M7 ports, built by EmbCC from their own
# sources, run on QEMU's MPS2 boards.
#
# freertos-cm3.sh does the Cortex-M3 port. These two save a task's FPU
# state as well: their PendSV handler stores s16-s31 when the task's
# EXC_RETURN says it used the FPU (`tst lr, #0x10; it eq; vstmdbeq`), and
# the core stacks s0-s15 and FPSCR itself. They also enable the FPU with
# `orr r1, r1, #( 0xf << 20 )` -- an immediate written as an expression,
# which the assembler refused until it learned to evaluate one.
#
# tests/golden/freertos-ports/main.c is freertos-cm3's program with two
# more tasks adding up floats under time slicing (BOARD_FPU); its output
# says whether both sums came out exact.
#
#   ARM_CM4F               on mps2-an386, a Cortex-M4 with FPv4-SP
#   ARM_CM7/r0p1           on mps2-an500, a Cortex-M7 (single precision
#                          used: thumbv7em-none-eabihf, -mfpu=fpv4-sp-d16)
#   ARM_CM33_NTZ/non_secure on mps2-an505, a Cortex-M33 with FPv5-SP, the
#                          ARMv8-M port (thumbv8m.main-none-eabihf): its
#                          context switch is portasm.c's, and it checks
#                          the stack limit registers (PSPLIM, MSPLIM)
#
# at -O0, -O1, -O2 and -Os, under -icount so that SysTick counts the
# guest's own instructions. The kernel is looked for in $EMBREF (default
# ~/EmbRef/FreeRTOS-Kernel); without it, or without qemu-system-arm, SKIP.
set -u
echo "TEST-MARKER freertos-ports"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
K=${EMBREF:-$HOME/EmbRef}/FreeRTOS-Kernel
[ -f "$K/tasks.c" ] && [ -f "$K/portable/GCC/ARM_CM4F/port.c" ] ||
    { echo "SKIP: no FreeRTOS-Kernel in ${EMBREF:-$HOME/EmbRef}"; exit 0; }
command -v "$QARM" >/dev/null 2>&1 || { echo "SKIP: no $QARM"; exit 0; }
d=tests/golden/freertos-ports
out=tests/golden/out/freertos-ports
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
for tr in thumbv7em-none-eabihf thumbv8m.main-none-eabihf; do
    sh tools/build-rt.sh $tr "$out/rt-$tr" > "$out/rt-$tr.log" 2>&1 ||
        { cat "$out/rt-$tr.log"; fail "librt for $tr"; }
done

ver=$(sed -n 's/^#define tskKERNEL_VERSION_NUMBER *"\(.*\)"/\1/p' "$K/include/task.h")
printf '%s\n' "FreeRTOS $ver on EmbCC" 'got 1' 'got 2' 'got 3' 'got 4' 'got 5' \
    'sum 15' 'waited enough 1' 'timer 1' 'fpu 1' 'done' > "$out/want.txt"
n=0
# port  machine  cpu  triple  -mfpu  UART  linker script  ARMv8-M
for cfg in \
    "ARM_CM4F mps2-an386 cortex-m4 thumbv7em-none-eabihf fpv4-sp-d16 0x40004000u mps2 0" \
    "ARM_CM7/r0p1 mps2-an500 cortex-m7 thumbv7em-none-eabihf fpv4-sp-d16 0x40004000u mps2 0" \
    "ARM_CM33_NTZ/non_secure mps2-an505 cortex-m33 thumbv8m.main-none-eabihf fpv5-sp-d16 0x40200000u mps2-an505 1"
do
    set -- $cfg
    port=$1 machine=$2 cpu=$3 tr=$4 fpu=$5 uart=$6 ld=$7 v8=$8
    tag=$(echo "$port" | tr '/' '-')
    T="--target=$tr -mfpu=$fpu"
    B="-DBOARD_UART=$uart -DBOARD_FPU=1 -DBOARD_CLOCK_HZ=25000000UL -DBOARD_PRIO_BITS=3"
    B="$B -DBOARD_V8M=$v8 -DBOARD_CMSIS_NAMES=$v8"
    srcs="$K/portable/GCC/$port/port.c"
    [ "$v8" = 1 ] && srcs="$srcs $K/portable/GCC/$port/portasm.c"
    for O in -O0 -O1 -O2 -Os; do
        o=$out/$tag${O#-O}; mkdir -p "$o"
        objs=
        for f in "$d/startup.c" "$d/main.c" "$K/tasks.c" "$K/queue.c" \
                 "$K/list.c" "$K/timers.c" "$K/portable/MemMang/heap_4.c" \
                 $srcs; do
            b=$(basename "$f" .c)
            "$EMBCC" $T $O $B -I"$d" -I"$K/include" -I"$K/portable/GCC/$port" \
                -c "$f" -o "$o/$b.o" 2> "$o/$b.err" ||
                { head -5 "$o/$b.err"; fail "$tag $O: $b.c did not compile"; }
            objs="$objs $o/$b.o"
        done
        "$EMBCC" $T -nostdlib -T "$d/$ld.ld" $objs "$out/rt-$tr/librt.a" \
            -o "$o/fw.elf" 2> "$o/link.err" ||
            { cat "$o/link.err"; fail "$tag $O: the link"; }
        sh tests/harness/qrun.sh 20 --until done "$QARM" -M $machine \
            -cpu $cpu -nographic -icount shift=2 -kernel "$o/fw.elf" \
            > "$o/run.txt" 2>&1
        tr -d '\r' < "$o/run.txt" | sed -n '1,/^done$/p' > "$o/got.txt"
        cmp -s "$o/got.txt" "$out/want.txt" || {
            echo "--- got"; cat "$o/run.txt"; echo "--- want"; cat "$out/want.txt"
            fail "$tag $O: FreeRTOS did not run as written"; }
        n=$((n + 1))
    done
    echo "freertos-ports $port on $machine: tasks, a queue, a mutex, a timer and FPU tasks run at -O0 -O1 -O2 -Os"
done
echo "freertos-ports: $n runs"
