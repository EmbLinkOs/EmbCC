#!/bin/sh
# Run one ColdFire harness image on QEMU's mcf5208evb board and pass the
# UART through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# -cpu m5208: ColdFire ISA_A+ with the hardware divide and no FPU, so a
# floating-point instruction in soft-float code traps (the harness reports
# it). -kernel loads the ELF and starts at its entry. UART0 is the first
# serial port. The image ends by printing `==EXIT n ==`, and qrun.sh
# --until stops QEMU at it; its timeout bounds an image that never prints
# it. The pattern is the WHOLE sentinel (mips/run.sh says why).
set -u
here=$(dirname "$0")
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==" \
    "${EMBCC_QEMU_M68K:-qemu-system-m68k}" -M mcf5208evb -cpu m5208 \
    -display none -monitor none -serial stdio -kernel "$1" 2>/dev/null
exit 0
