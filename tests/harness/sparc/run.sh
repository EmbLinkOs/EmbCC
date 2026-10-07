#!/bin/sh
# Run one SPARC harness image on QEMU's leon3_generic board and pass the
# APBUART through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# -kernel loads the ELF into RAM and starts it from QEMU's own boot
# loader. The image ends by printing `==EXIT n ==` and then executing
# `ta 0` with traps disabled, which QEMU's LEON3 takes as a shutdown;
# qrun.sh --until stops at the sentinel in any case, and its timeout
# bounds an image that never prints it. The pattern is the WHOLE
# sentinel: the UART writes a byte at a time.
set -u
QEMU=${EMBCC_QEMU_SPARC:-qemu-system-sparc}
"$(dirname "$0")/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==" \
    "$QEMU" -M leon3_generic -m 64 -display none -monitor none \
    -serial stdio -no-reboot -kernel "$1" 2>/dev/null
exit 0
