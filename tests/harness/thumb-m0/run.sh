#!/bin/sh
# Run one ARMv6-M harness image on QEMU's micro:bit (Cortex-M0) and pass
# its UART through to stdout. The exit status is the program's own, read
# from the `==EXIT n==` line its _exit() prints (io.c): 125 for a fault,
# 124 when nothing was printed before the timeout.
#
#   usage: run.sh IMAGE.elf
#
# The image never exits by itself, so qrun.sh --until stops QEMU as soon
# as the line appears.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
# EMBCC_M0_SRAM raises the SoC's SRAM (bytes) for programs larger than the
# part's 16 KiB; boot.c must then be built with the matching -DSRAM_TOP.
sram=
[ -n "${EMBCC_M0_SRAM:-}" ] && sram="-global nrf51-soc.sram-size=$EMBCC_M0_SRAM"
# shellcheck disable=SC2086
o=$("$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until '==EXIT ' \
    "$QEMU" -M microbit $sram -nographic -kernel "$1" 2>/dev/null)
printf '%s\n' "$o" | sed '/^==EXIT /d'
code=$(printf '%s\n' "$o" | sed -n 's/^==EXIT \(-*[0-9]*\)==.*/\1/p' | tail -1)
[ -n "$code" ] || exit 124
exit $((code & 255))
