#!/bin/sh
# The o32 calling convention BIG-endian, against clang across the call:
# mips-abi.sh's caller/callee pairs compiled for mips-none-elf by EmbCC
# and for mips-unknown-elf by clang, in every pairing, run on QEMU's
# big-endian malta. What the byte order changes in o32 is here: a long
# long's or double's high word in the lower register of its pair (and the
# lower stack word), results high word in v0, and a composite shorter than
# a word left-justified in its register (mips-abi.h).
MIPS_ABI_BE=1 exec sh "$(dirname "$0")/mips-abi.sh"
