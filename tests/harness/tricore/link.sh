#!/bin/sh
# Link one TriCore harness image: the startup, the output and the program
# under test, into something QEMU's tricore_testboard loads with -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Linked at 0x80000000, the board's 2 MiB code RAM, as a RAM image (no
# -Tdata: .data is where it is addressed). The context-save areas -- one
# per call in progress, 64 bytes each -- are the top 512 KiB of the same
# RAM, 8192 of them: a CSA link word can only name the first 4 MiB of a
# 256 MiB segment, which rules out the board's larger data RAM at
# 0xa1000000. The stack's top is that data RAM's end, 0xa1400000, growing
# down 4 MiB. -Tstack and --csa make embld emit the stub that sets A10,
# links the CSAs and jumps to _start, which C cannot do.
#
# EMBCC_TRICORE_HARNESS says where boot.o and io.o were built; it
# defaults to beside this script. A test that runs CONCURRENTLY with
# others must set it to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_TRICORE_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x80000000 -Tstack 0xa1400000 \
    --csa 0x80180000:0x80200000 "$H/boot.o" "$H/io.o" "$@" -o "$out"
