#!/bin/sh
# Link one ARMv8-M (Cortex-M33) harness image for QEMU's mps2-an505.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Everything lives in the board's ZBT SSRAM1, and at its SECURE alias:
#
#   0x10000000  code          0x10100000  the writable image
#   0x10200000  the stack top (boot.c), growing down
#
# The secure alias is not a detail. On an Armv8-M with TrustZone the same
# physical memory appears at 0x00000000 as NON-SECURE and at 0x10000000 as
# secure, and the core comes out of reset in the SECURE state -- so an image
# linked at 0 is fetched from non-secure memory by a secure core, and QEMU
# answers "Lockup: can't escalate 3 to HardFault" before the first
# instruction retires. That is the one thing about this board that differs
# from every other Cortex-M harness here.
#
# EMBCC_M33_HARNESS says where boot.o and io.o were built; it defaults to
# beside this script. A test that runs CONCURRENTLY with others must set it
# to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_M33_HARNESS:-$here}
"${EMBLD:-./embld}" -e reset -Ttext 0x10000000 -Tdata 0x10100000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
