#!/bin/sh
# Run one ARMv7-A harness image on QEMU's virt board (a Cortex-A15) and
# pass the UART through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# -kernel loads the ELF into RAM and starts it at its entry in ARM state.
# The image ends by printing `==EXIT n ==` and exiting through semihosting,
# which ends QEMU at once; qrun.sh --until stops at the sentinel in any
# case, and its timeout bounds an image that never prints it. The pattern
# is the WHOLE sentinel, for the reason tests/harness/mips/run.sh gives.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==" \
    "$QEMU" -M virt -cpu cortex-a15 -m 128 -nographic -monitor none \
    -semihosting -kernel "$1" 2>/dev/null
exit 0
