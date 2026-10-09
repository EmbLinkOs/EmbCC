#!/bin/sh
# What the MIPS32 backend COMPUTES BIG-endian: every program in tests/exec
# at -O0, -O1, -O2 and -Os, compiled for mips-none-elf, linked by embld
# with lib/libc and lib/rt built for it, and run on QEMU's big-endian
# malta (qemu-system-mips). The same script as mips-exec.sh, which holds
# the not-applicable list and the LP64 programs refereed against clang
# (here clang for mips-unknown-elf). This is the main proof that byte
# order is right end to end: a type pun the optimizer or a data writer
# gets backwards only shows when the program runs
# (docs/internals/big-endian.md).
MIPS_EXEC_BE=1 exec sh "$(dirname "$0")/mips-exec.sh"
