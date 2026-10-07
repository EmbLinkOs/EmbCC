#!/bin/sh
# Run one MIPS64 harness image on QEMU's malta board -- little- or
# big-endian, as the image is -- and pass the UART through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# The startup and the output are tests/harness/mips's boot.c and io.c,
# which address KSEG0/KSEG1 sign-extended at 64 bits. -cpu 5KEc: MIPS64
# Release 2 with NO floating-point unit, so a floating-point instruction
# in soft-float code traps. -kernel loads the ELF64 image into RAM at its
# KSEG0 link address (0xffffffff80100000) and starts it from QEMU's own
# reset code. The FPGA UART is the third serial port. The image ends by
# printing `==EXIT n ==` and resetting the board, which -no-reboot makes
# QEMU's exit; qrun.sh --until stops at the whole sentinel in any case,
# and its timeout bounds an image that never prints it.
set -u
here=$(dirname "$0")
# EI_DATA, the ELF header's sixth byte: 2 for a big-endian image
# (mips64-none-elf), which runs on qemu-system-mips64.
if [ "$(od -An -tu1 -j5 -N1 "$1" 2>/dev/null | tr -d ' ')" = 2 ]; then
    QEMU=${EMBCC_QEMU_MIPS64EB:-qemu-system-mips64}
else
    QEMU=${EMBCC_QEMU_MIPS64:-qemu-system-mips64el}
fi
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==" \
    "$QEMU" -M malta -cpu 5KEc -m 64 -display none -monitor none \
    -serial null -serial null -serial stdio -no-reboot -kernel "$1" \
    2>/dev/null
exit 0
