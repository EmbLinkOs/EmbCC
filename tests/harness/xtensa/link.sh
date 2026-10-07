#!/bin/sh
# Link one Xtensa harness image: the startup, the simcall output and the
# program under test, into something QEMU's sim machine loads with -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Linked at 0x60010000, in the de212's system RAM (0x60000000, 128 MiB
# under -m 128): above the 0x680 bytes of exception vectors the harness
# copies to VECBASE, 0x60000000. The stack's top is 0x60800000, 8 MiB in,
# growing down; -Tstack makes embld emit the stub that sets sp and PS and
# calls _start with callx8, which is the one thing C cannot do.
#
# EMBCC_XTENSA_HARNESS says where boot.o and io.o were built; it defaults
# to beside this script.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_XTENSA_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x60010000 -Tstack 0x60800000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
