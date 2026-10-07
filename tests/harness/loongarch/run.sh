#!/bin/sh
# Run one LoongArch64 harness image on QEMU's virt board and pass the UART
# through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# -kernel loads the ELF into RAM at its addresses and starts it at the
# entry in direct address mode, with no firmware in front. The UART the
# harness writes is the first serial port. The image ends by printing
# `==EXIT n ==` and powering the board off, which ends QEMU; qrun.sh
# --until stops at the sentinel in any case, and its timeout bounds an
# image that never prints it. The pattern is the WHOLE sentinel: the UART
# writes a byte at a time, and stopping at `==EXIT` alone once killed
# QEMU between it and the status (tests/harness/mips/run.sh).
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_LOONGARCH:-qemu-system-loongarch64}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==" \
    "$QEMU" -M virt -m 64 -display none -monitor none -serial stdio \
    -no-reboot -kernel "$1" 2>/dev/null
exit 0
