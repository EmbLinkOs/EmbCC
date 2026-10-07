#!/bin/sh
# Run one Xtensa harness image on QEMU's sim machine with the de212 core
# and pass its console through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# -cpu de212: an LX6 with windowed registers, MUL32, DIV32 and no FPU or
# MMU (docs/internals/xtensa-plan.md). The image is loaded by the generic
# loader with cpu-num=0, which also starts the CPU at its ELF entry: the
# sim machine's -kernel loads it too, but its reset handler then puts the
# CPU back at the core's reset vector (0x50000000), where nothing is.
# -semihosting enables the simcalls
# the harness writes and exits with. The image ends by printing
# `==EXIT n ==` and the exit simcall; qrun.sh --until stops at the
# sentinel in any case, and its timeout bounds an image that never prints
# it. A fault prints `==FAULT cause C pc P addr A ==` and exits 99.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_XTENSA:-qemu-system-xtensa}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==\|==FAULT" \
    "$QEMU" -M sim -cpu de212 -m 128 -semihosting -display none \
    -monitor none -device loader,file="$1",cpu-num=0 2>/dev/null
exit 0
