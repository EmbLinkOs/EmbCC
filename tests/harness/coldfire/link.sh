#!/bin/sh
# Link one ColdFire harness image: the startup, the UART output and the
# program under test, into something QEMU's mcf5208evb loads with -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Linked at 0x40100000, 1 MiB into the board's SDRAM (0x40000000, 128 MiB);
# the first megabyte holds the vector table boot.c builds. The stack's top
# is 0x48000000, the end of SDRAM, growing down; -Tstack makes embld emit
# the stub that sets %sp and jumps to _start, which is the one thing C
# cannot do.
#
# EMBCC_CF_HARNESS says where boot.o and io.o were built; it defaults to
# beside this script. A test that runs CONCURRENTLY with others must set it
# to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_CF_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x40100000 -Tstack 0x48000000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
