#!/bin/sh
# Link one PowerPC harness image: the startup, the UART output and the
# program under test, into something QEMU's ppce500 board loads with
# -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Linked at 0x00100000, inside the 64 MiB QEMU maps 1:1 at reset. The
# stack's top is 0x00800000, growing down; -Tstack makes embld emit the stub
# that sets r1 and branches to _start, which is the one thing C cannot do.
#
# EMBCC_PPC_HARNESS says where boot.o and io.o were built; it defaults to
# beside this script. A test that runs CONCURRENTLY with others must set it
# to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_PPC_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x00100000 -Tstack 0x00800000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
