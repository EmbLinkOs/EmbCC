#!/bin/sh
# Link one LoongArch64 harness image: the startup, the UART output and the
# program under test, into something QEMU's virt board loads with -kernel.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Linked at 0x1000000 (16 MiB), in the low RAM QEMU's virt board maps at 0
# and runs in direct address mode -- above the 2 MiB QEMU fills with its
# boot information and the device tree (`info roms`: 0-0x100000 and
# 0x100000-0x200000), which an image linked at 0x200000 overlapped as soon
# as embld put its ELF header in the first page. The stack's top is
# 0x3000000, 32 MiB above, growing down; -Tstack makes embld emit the
# stub that sets sp and jumps to _start, which is the one thing C cannot
# do. No -Tdata: the whole image is loaded into RAM, so .data is already
# where it is addressed.
#
# EMBCC_LOONGARCH_HARNESS says where boot.o and io.o were built; it
# defaults to beside this script. A test that runs CONCURRENTLY with
# others must set it to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_LOONGARCH_HARNESS:-$here}
"${EMBLD:-./embld}" -e _start -Ttext 0x1000000 -Tstack 0x3000000 \
    "$H/boot.o" "$H/io.o" "$@" -o "$out"
