#!/bin/sh
# Run one PowerPC harness image on QEMU's ppce500 board and pass the UART
# through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# -kernel with no -bios runs the ELF itself (QEMU's own table: -kernel
# without -bios is the kernel, not u-boot). The default CPU, an e500v2, has
# no classic FPU, so a floating-point instruction in soft-float code traps
# and the harness reports it. The image ends by printing `==EXIT n ==` and
# resetting the board, which -no-reboot makes QEMU's exit; qrun.sh --until
# stops at the sentinel in any case, and its timeout bounds an image that
# never prints it. The pattern is the WHOLE sentinel (mips/run.sh says why).
set -u
here=$(dirname "$0")
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==" \
    "${EMBCC_QEMU_PPC:-qemu-system-ppc}" -M ppce500 -m 128 -display none \
    -monitor none -serial stdio -no-reboot -kernel "$1" 2>/dev/null
exit 0
