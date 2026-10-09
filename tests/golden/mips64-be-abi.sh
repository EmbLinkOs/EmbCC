#!/bin/sh
# The n64 calling convention BIG-endian: tests/golden/mips64-abi.sh's
# pairs, EmbCC for mips64-none-elf against clang for mips64-unknown-elf, on
# qemu-system-mips64. Where the byte order shows: a short composite
# left-justified in its register, a long double's and an __int128's high
# doubleword in the lower-numbered register, a float field returned in
# the low half of its own register.
MIPS64_ABI_BE=1 exec sh "$(dirname "$0")/mips64-abi.sh"
