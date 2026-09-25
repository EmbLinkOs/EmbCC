#!/bin/sh
# Run one ARMv7-M harness image on QEMU's Stellaris LM3S6965 (Cortex-M3)
# and pass the UART through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# The image never exits: a reset handler is the bottom of the call stack
# on this machine, and stopping QEMU cleanly would take a semihosting
# `bkpt`, which is assembly this compiler cannot yet emit. So the run is
# bounded by ../qrun.sh's timeout and judged by its OUTPUT — which is
# why every harness program ends by printing a sentinel the caller looks
# for. Being killed is the expected end, so the status is not the
# answer and this always exits 0.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" "$QEMU" \
    -M lm3s6965evb -cpu cortex-m3 -nographic -kernel "$1" 2>/dev/null
exit 0
