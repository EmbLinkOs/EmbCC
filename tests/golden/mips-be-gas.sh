#!/bin/sh
# mips-gas.sh, BIG-endian: the same checks for mips-none-elf (llvm-mc and clang
# for mips-unknown-elf, qemu-system-mips). docs/internals/big-endian.md.
MIPS_BE=1 exec sh "$(dirname "$0")/mips-gas.sh"
