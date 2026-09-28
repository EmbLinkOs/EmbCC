#!/bin/sh
# Run one ARMv8-M harness image on QEMU's mps2-an505 (Cortex-M33) and pass
# the UART through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# The image never exits: a reset handler is the bottom of the call stack on
# this machine and stopping QEMU cleanly would take a semihosting `bkpt`. So
# the run is bounded by ../qrun.sh's timeout and judged by its OUTPUT, which
# is why every harness program ends by printing a sentinel. Being killed is
# the expected end, so the status is not the answer and this always exits 0.
#
# QEMU's stderr is KEPT. This board refuses an image linked at the
# non-secure alias with one clear line, and discarding it makes that look
# exactly like a program that printed nothing.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
# EMBCC_QEMU_UNTIL: end the run when the image prints this sentinel rather
# than when the timeout expires -- see ../qrun.sh. Optional; without it the
# behaviour is what it always was.
if [ -n "${EMBCC_QEMU_UNTIL:-}" ]; then
    "$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "$EMBCC_QEMU_UNTIL" \
        "$QEMU" -M mps2-an505 -cpu cortex-m33 -nographic -kernel "$1"
else
    "$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" "$QEMU" \
        -M mps2-an505 -cpu cortex-m33 -nographic -kernel "$1"
fi
exit 0
