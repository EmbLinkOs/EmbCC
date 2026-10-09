#!/bin/sh
# Link one MIPS64 harness image: the startup, the UART output and the
# program under test, into something QEMU's malta board loads with -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Linked at 0xffffffff80100000, KSEG0 sign-extended (the n64 view of the
# window MIPS32 images use at 0x80100000), the stack's top 7 MB above;
# -Tstack makes embld emit the stub that sets sp and jumps to _start.
#
# EMBCC_MIPS64_HARNESS says where boot.o and io.o (tests/harness/mips's
# sources, built for the 64-bit triple) were built; it defaults to beside
# this script. A test that runs CONCURRENTLY with others must set it to
# its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_MIPS64_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0xffffffff80100000 \
    -Tstack 0xffffffff80800000 "$H/boot.o" "$H/io.o" "$@" -o "$out"
