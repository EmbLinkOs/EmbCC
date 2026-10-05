#!/bin/sh
# Link one ARMv6-M harness image: the startup, the UART and the program
# under test, at the micro:bit's flash (0) and SRAM (0x20000000).
#
#   usage: link.sh OUT.elf OBJ-OR-ARCHIVE...
#
# EMBCC_THUMB_M0_HARNESS says where boot.o and io.o were built (by default
# beside this script); a test running concurrently with others sets it to
# its own output directory. Archives (libc.a, librt.a) go last.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_THUMB_M0_HARNESS:-$here}
"${EMBLD:-./embld}" -e reset -Ttext 0x0 -Tdata 0x20000000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
