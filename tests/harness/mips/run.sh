#!/bin/sh
# Run one MIPS harness image on QEMU's malta board and pass the UART
# through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# -cpu 24Kc: MIPS32r2 with NO floating-point unit, so a floating-point
# instruction in soft-float code traps rather than quietly working.
# -kernel loads the ELF into RAM and starts it from QEMU's own reset code.
# The FPGA UART the harness writes is the third serial port. The image
# ends by printing `==EXIT n ==` and resetting the board, which -no-reboot
# makes QEMU's exit; qrun.sh --until stops at the sentinel in any case,
# and its timeout bounds an image that never prints it. The pattern is the
# WHOLE sentinel: the UART writes a byte at a time, and stopping at
# `==EXIT` alone once killed QEMU between it and the status, on a busy
# machine, so a passing program read as one that never exited.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==" \
    "$QEMU" -M malta -cpu 24Kc -m 64 -display none -monitor none \
    -serial null -serial null -serial stdio -no-reboot -kernel "$1" \
    2>/dev/null
exit 0
