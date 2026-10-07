#!/bin/sh
# Link one SPARC harness image: the startup, the UART output and the
# program under test, into something QEMU's leon3_generic loads with
# -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Linked at 0x40000000, the start of the board's RAM; the stack's top is
# 0x40800000, 8 MB above, growing down. -Tstack makes embld emit the stub
# that sets %sp and jumps to _start, which C cannot do. No -Tdata: the
# whole image is loaded into RAM, so .data is already where it is
# addressed.
#
# EMBCC_SPARC_HARNESS says where boot.o and io.o were built; it defaults
# to beside this script. A test that runs CONCURRENTLY with others must
# set it to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_SPARC_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x40000000 -Tstack 0x40800000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
