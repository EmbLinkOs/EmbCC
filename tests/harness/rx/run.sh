#!/bin/sh
# Run one RX harness image on QEMU's gdbsim-r5f562n8 and pass SCI0 through
# to stdout.
#
#   usage: run.sh IMAGE.bin      (the raw image link.sh writes)
#
# -kernel copies the raw image to 0x01800000 and starts there; the image
# ends by printing `==EXIT n ==`, at which tests/harness/qrun.sh --until
# stops QEMU (nothing in the gdbsim powers it off). The pattern is the
# WHOLE sentinel, so a run killed between `==EXIT` and the status is not
# read as one that never exited.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_RX:-qemu-system-rx}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==" \
    "$QEMU" -M gdbsim-r5f562n8 -display none -monitor none \
    -serial stdio -kernel "$1" 2>/dev/null
exit 0
