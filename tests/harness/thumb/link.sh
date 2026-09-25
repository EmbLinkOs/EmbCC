#!/bin/sh
# Link one ARMv7-M harness image: the startup, the UART output and the
# program under test, into a firmware layout QEMU's -kernel accepts.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Flash at 0, SRAM at 0x20000000. -Tdata is what makes embld store the
# writable segment after the text and address it in RAM, and provide the
# __data_load/__bss_start brackets boot.c copies and zeroes with.
set -eu
out=$1; shift
here=$(dirname "$0")
"${EMBLD:-./embld}" -e reset -Ttext 0x0 -Tdata 0x20000000 \
    "$here/boot.o" "$here/io.o" "$@" -o "$out"
