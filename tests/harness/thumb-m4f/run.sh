#!/bin/sh
# Run one ARMv7-M harness image on QEMU's Stellaris LM3S6965 (Cortex-M3)
# and pass the UART through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# The image never exits: a reset handler is the bottom of the call stack
# on this machine. When main returns, boot.c prints `==EXIT==` on the
# UART, and QEMU is stopped as soon as that appears (../qrun.sh
# --until); the line is taken out of the output, byte for byte, so a
# caller sees what the program printed. An image that never returns from
# main is bounded by the timeout, as every run was before -- a minute
# each on CI. The run is judged by its OUTPUT, so this always exits 0.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until '==EXIT==' "$QEMU" \
    -M mps2-an386 -cpu cortex-m4 -nographic -kernel "$1" 2>/dev/null |
    perl -0pe 's/==EXIT==\n?\z//'
exit 0
