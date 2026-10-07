#!/bin/sh
# Link one ARMv8-M Baseline harness image for QEMU's mps2-an505.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Code at the SECURE alias of the ZBT SSRAM1, 0x10000000, the writable image
# at 0x10100000 and the stack from 0x10200000 down -- as ../thumb-m33/link.sh,
# whose comment says why the alias matters.
#
# EMBCC_M23_HARNESS says where boot.o and io.o were built; it defaults to
# beside this script.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_M23_HARNESS:-$here}
"${EMBLD:-./embld}" -e reset -Ttext 0x10000000 -Tdata 0x10100000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
