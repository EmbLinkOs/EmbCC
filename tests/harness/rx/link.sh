#!/bin/sh
# Link one RX harness image: the startup, the SCI output and the program
# under test, into an ELF and the raw image QEMU's gdbsim loads.
#
#   usage: link.sh OUT.elf PROGRAM.o...     -> OUT.elf and OUT.bin
#
# Linked at 0x01800000, where -kernel copies a raw image (the second half
# of the 16 MiB SDRAM at 0x01000000); the stack's top is the SDRAM's end,
# 0x02000000. -Tstack makes embld emit the stub that sets r0 and jumps to
# _start (the object's __start: RX C names carry an underscore).
#
# EMBCC_RX_HARNESS says where boot.o and io.o were built; it defaults to
# beside this script.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_RX_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x01800000 -Tstack 0x02000000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
"${EMBCC_OBJCOPY:-llvm-objcopy}" -O binary "$out" "${out%.elf}.bin"
