#!/bin/sh
# Run one Cortex-M7 harness image on QEMU's mps2-an500 (a Cortex-M7 with
# the double-precision FPU) and pass the UART through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# The image never exits on its own -- the reset handler is the bottom of
# the stack and the board has no semihosting call to stop QEMU with -- so
# the run ends when the guest prints io.c's "==EXIT" sentinel or the
# program's own EMBCC_QEMU_UNTIL text, and otherwise at ../qrun.sh's
# timeout. Being killed is the expected end, so this always exits 0 and
# the OUTPUT is the answer.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" \
    --until "${EMBCC_QEMU_UNTIL:-==EXIT}" \
    "$QEMU" -M mps2-an500 -cpu cortex-m7 -nographic -kernel "$1" 2>/dev/null
exit 0
