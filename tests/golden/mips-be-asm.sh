#!/bin/sh
# MIPS inline assembly, file-scope blocks and -S, BIG-endian: mips-asm.sh
# run for mips-none-elf -- the vocabulary's bytes against llvm-mc's for
# mips-unknown-elf, the asm program on qemu-system-mips, the refusals, and
# -S reassembled by llvm-mc to -c's program.
MIPS_BE=1 exec sh "$(dirname "$0")/mips-asm.sh"
