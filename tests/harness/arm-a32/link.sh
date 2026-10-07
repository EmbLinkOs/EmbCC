#!/bin/sh
# Link one ARMv7-A harness image: the startup, the UART output and the
# program under test, into something QEMU's virt board loads with -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Linked at 0x40100000, 1 MB into virt's RAM (which starts at 0x40000000),
# clear of the device tree QEMU places at its base. The stack's top is
# 0x40800000, growing down; -Tstack makes embld emit the A32 stub that sets
# sp and jumps to _start, which is the one thing C cannot do. No -Tdata:
# the whole image is loaded into RAM, so .data is already where it is
# addressed.
#
# EMBCC_A32_HARNESS says where boot.o and io.o were built; it defaults to
# beside this script. A test that runs CONCURRENTLY with others must set it
# to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_A32_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x40100000 -Tstack 0x40800000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
