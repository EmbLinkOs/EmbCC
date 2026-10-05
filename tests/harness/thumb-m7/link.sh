#!/bin/sh
# Link one Cortex-M7 harness image: the startup, the UART output and the
# program under test (and, for the exec corpus, exec.o and lib/libc), into
# the layout QEMU's -kernel loads on mps2-an500.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Code at 0 (ZBT SSRAM1), data at 0x20000000 (SSRAM2/3), exactly as the
# Cortex-M4F harness lays them out: -Tdata makes embld put the writable
# segment after the text and address it in RAM, with the __data_load and
# __bss_* brackets boot.c copies and zeroes with.
#
# EMBCC_M7_HARNESS says where boot.o and io.o were built, defaulting to
# beside this script. A test that runs alongside others sets it to its own
# output directory: tests/run.sh runs golden tests in parallel and each may
# write only there.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_M7_HARNESS:-$here}
"${EMBLD:-./embld}" -e reset -Ttext 0x0 -Tdata 0x20000000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
