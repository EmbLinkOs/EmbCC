#!/bin/sh
# Link one ARMv7-M harness image: the startup, the UART output and the
# program under test, into a firmware layout QEMU's -kernel accepts.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Flash at 0, SRAM at 0x20000000. -Tdata is what makes embld store the
# writable segment after the text and address it in RAM, and provide the
# __data_load/__bss_start brackets boot.c copies and zeroes with.
#
# EMBCC_THUMB_HARNESS says where boot.o and io.o were built; it defaults
# to beside this script. A test that runs CONCURRENTLY with others must
# set it to its own output directory — tests/run.sh runs the golden
# tests in parallel on the understanding that each writes only inside
# tests/golden/out/<name>, and harness objects built beside the sources
# would be shared state between them.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_THUMB_HARNESS:-$here}
"${EMBLD:-./embld}" -e reset -Ttext 0x0 -Tdata 0x20000000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
