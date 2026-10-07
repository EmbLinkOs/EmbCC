#!/bin/sh
# Link one TriCore harness image: the startup, the output and the program
# under test, into something QEMU's tricore_testboard loads with -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# The text at 0x80000000, the board's 2 MiB code RAM; .data and .bss in
# the 4 MiB data RAM at 0xa1000000 (-Tdata: boot.c copies .data there from
# behind the text), the stack growing down from its top, 0xa1400000. Not
# data beside the code: QEMU invalidates the translated code of a page on
# every store to it, and a program whose hot globals shared a page with its
# hot code ran ten times slower (a fuzz seed timed out at -Os). The
# context-save areas -- one per call in progress, 64 bytes each -- are the
# top 512 KiB of the code RAM, 8192 of them: a CSA link word can only name
# the first 4 MiB of a 256 MiB segment, which rules out 0xa1000000.
# -Tstack and --csa make embld emit the stub that sets A10, links the CSAs
# and jumps to _start, which C cannot do.
#
# EMBCC_TRICORE_HARNESS says where boot.o and io.o were built; it
# defaults to beside this script. A test that runs CONCURRENTLY with
# others must set it to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_TRICORE_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x80000000 -Tdata 0xa1000000 \
    -Tstack 0xa1400000 \
    --csa 0x80180000:0x80200000 "$H/boot.o" "$H/io.o" "$@" -o "$out"
