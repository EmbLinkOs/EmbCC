#!/bin/sh
# Run one RISC-V harness image on QEMU's `virt` board and pass the UART
# through to stdout.
#
#   usage: run.sh IMAGE.elf [32|64]
#
# Unlike the ARMv7-M harness, this one EXITS: `virt` has a SiFive test
# device that boot.c writes to after main returns, so QEMU stops on its
# own and the timeout in ../qrun.sh is a backstop rather than the normal
# end of a run. The status is still not what a test judges -- the output
# is -- but a hung guest now costs the timeout only when something is
# actually wrong.
#
# -bios none: without it QEMU loads OpenSBI first, which prints a banner
# and hands control to the image in supervisor mode. The harness runs in
# machine mode and wants the UART to itself.
set -u
here=$(dirname "$0")
xlen=${2:-64}
QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv$xlen}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" "$QEMU" \
    -M virt -bios none -nographic -m 8 -kernel "$1" 2>/dev/null
exit 0
