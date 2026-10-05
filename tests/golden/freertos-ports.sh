#!/bin/sh
# FreeRTOS's Cortex-M4F, M7, M33 and RISC-V ports, built by EmbCC from their
# own sources, run on QEMU.
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
#   RISC-V                 on virt, RV32 and RV64 (riscv32-/riscv64-
#                          unknown-elf), the CLINT's timer: its context
#                          switch is portASM.S, which a .S assembles with
#                          the -I path to the chip-specific header, loads
#                          pxCurrentTCB with `lw sp, pxCurrentTCB`, masks
#                          interrupts with `csrc mstatus, 8`, and sizes
#                          its frame with expressions; port.c starts its
#                          interrupt stack with (StackType_t)&xISRStack[N]
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
QRV=${EMBCC_QEMU_RISCV:-qemu-system-riscv}
d=tests/golden/freertos-ports
out=tests/golden/out/freertos-ports
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
for tr in thumbv7em-none-eabihf thumbv8m.main-none-eabihf \
          riscv32-unknown-elf riscv64-unknown-elf; do
    sh tools/build-rt.sh $tr "$out/rt-$tr" > "$out/rt-$tr.log" 2>&1 ||
        { cat "$out/rt-$tr.log"; fail "librt for $tr"; }
done

ver=$(sed -n 's/^#define tskKERNEL_VERSION_NUMBER *"\(.*\)"/\1/p' "$K/include/task.h")
printf '%s\n' "FreeRTOS $ver on EmbCC" 'got 1' 'got 2' 'got 3' 'got 4' 'got 5' \
    'sum 15' 'waited enough 1' 'timer 1' > "$out/want-common.txt"
{ cat "$out/want-common.txt"; echo 'fpu 1'; echo done; } > "$out/want-fpu.txt"
{ cat "$out/want-common.txt"; echo done; } > "$out/want.txt"
n=0
# port  board  triple  -mfpu  UART  linker script  kind (v7, v8 or rv)
for cfg in \
    "ARM_CM4F mps2-an386:cortex-m4 thumbv7em-none-eabihf fpv4-sp-d16 0x40004000u mps2 v7" \
    "ARM_CM7/r0p1 mps2-an500:cortex-m7 thumbv7em-none-eabihf fpv4-sp-d16 0x40004000u mps2 v7" \
    "ARM_CM33_NTZ/non_secure mps2-an505:cortex-m33 thumbv8m.main-none-eabihf fpv5-sp-d16 0x40200000u mps2-an505 v8" \
    "RISC-V virt riscv32-unknown-elf - 0x10000000u riscv rv" \
    "RISC-V virt riscv64-unknown-elf - 0x10000000u riscv rv"
do
    set -- $cfg
    port=$1 board=$2 tr=$3 fpu=$4 uart=$5 ld=$6 kind=$7
    P=$K/portable/GCC/$port
    tag=$(echo "$port-$tr" | tr '/' '-')
    case $kind in
    rv)
        command -v ${QRV}32 >/dev/null 2>&1 || { echo "SKIP $tag: no ${QRV}32"; continue; }
        T="--target=$tr"
        B="-DBOARD_UART=$uart -DBOARD_FPU=0 -DBOARD_RISCV=1 -DBOARD_CLOCK_HZ=10000000UL"
        B="$B -DBOARD_PRIO_BITS=3 -I$P/chip_specific_extensions/RISCV_MTIME_CLINT_no_extensions"
        boot="$d/riscv-start.S $d/riscv-startup.c"
        srcs="$P/port.c $P/portASM.S"
        xl=${tr#riscv}; xl=${xl%%-*}
        qemu="${QRV}$xl -M virt -bios none -m 8"
        want=$out/want.txt ;;
    *)
        T="--target=$tr -mfpu=$fpu"
        v8=0; [ $kind = v8 ] && v8=1
        B="-DBOARD_UART=$uart -DBOARD_FPU=1 -DBOARD_CLOCK_HZ=25000000UL -DBOARD_PRIO_BITS=3"
        B="$B -DBOARD_V8M=$v8 -DBOARD_CMSIS_NAMES=$v8"
        boot=$d/startup.c
        srcs="$P/port.c"
        [ $v8 = 1 ] && srcs="$srcs $P/portasm.c"
        qemu="$QARM -M ${board%%:*} -cpu ${board#*:}"
        want=$out/want-fpu.txt ;;
    esac
    for O in -O0 -O1 -O2 -Os; do
        o=$out/$tag${O#-O}; mkdir -p "$o"
        objs=
        for f in $boot "$d/main.c" "$K/tasks.c" "$K/queue.c" \
                 "$K/list.c" "$K/timers.c" "$K/portable/MemMang/heap_4.c" \
                 $srcs; do
            b=$(basename "$f")
            "$EMBCC" $T $O $B -I"$d" -I"$K/include" -I"$P" \
                -c "$f" -o "$o/$b.o" 2> "$o/$b.err" ||
                { head -5 "$o/$b.err"; fail "$tag $O: $b did not compile"; }
            objs="$objs $o/$b.o"
        done
        "$EMBCC" $T -nostdlib -T "$d/$ld.ld" $objs "$out/rt-$tr/librt.a" \
            -o "$o/fw.elf" 2> "$o/link.err" ||
            { cat "$o/link.err"; fail "$tag $O: the link"; }
        sh tests/harness/qrun.sh 20 --until done $qemu -nographic \
            -icount shift=2 -kernel "$o/fw.elf" > "$o/run.txt" 2>&1
        tr -d '\r' < "$o/run.txt" | sed -n '1,/^done$/p' > "$o/got.txt"
        cmp -s "$o/got.txt" "$want" || {
            echo "--- got"; cat "$o/run.txt"; echo "--- want"; cat "$want"
            fail "$tag $O: FreeRTOS did not run as written"; }
        n=$((n + 1))
    done
    echo "freertos-ports $port, $tr, on ${board%%:*}: runs at -O0 -O1 -O2 -Os"
done
echo "freertos-ports: $n runs"
