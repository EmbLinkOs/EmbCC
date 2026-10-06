#!/bin/sh
# Link one MIPS harness image: the startup, the UART output and the
# program under test, into something QEMU's malta board loads with -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Linked at 0x80100000, in KSEG0: cached, unmapped RAM, above what QEMU
# keeps for its own boot environment (0x80002000). The stack's top is
# 0x80800000, 7 MB above, growing down; -Tstack makes embld emit the stub
# that sets sp and jumps to _start, which is the one thing C cannot do.
# No -Tdata: the whole image is loaded into RAM, so .data is already
# where it is addressed.
#
# EMBCC_MIPS_HARNESS says where boot.o and io.o were built; it defaults
# to beside this script. A test that runs CONCURRENTLY with others must
# set it to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_MIPS_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x80100000 -Tstack 0x80800000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
