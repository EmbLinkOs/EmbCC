#!/bin/sh
# Link one RISC-V harness image: the startup, the UART output and the
# program under test, into something QEMU's `virt` board will run.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# RAM at 0x80000000, which is where `virt` puts it and where `-kernel`
# loads. The stack goes at the top of the 8MB QEMU is given, growing
# down; -Tstack is what makes embld emit the four instructions that set
# sp before reaching _start, which is the one thing C cannot do here.
#
# -Tdata is not used: a `virt` image is loaded wholly into RAM, so there
# is no flash-to-RAM copy to arrange and .data is already where it is
# addressed. boot.c's copy loop then moves nothing, which is correct and
# costs two compares.
#
# EMBCC_RISCV_HARNESS says where boot.o and io.o were built; it defaults
# to beside this script. A test that runs CONCURRENTLY with others must
# set it to its own output directory -- tests/run.sh runs the golden
# tests in parallel on the understanding that each writes only inside
# tests/golden/out/<name>.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_RISCV_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x80000000 -Tstack 0x80800000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
