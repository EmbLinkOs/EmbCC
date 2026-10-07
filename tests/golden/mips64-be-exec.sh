#!/bin/sh
# What the MIPS64 backend COMPUTES BIG-endian: every program in tests/exec
# at -O0, -O1, -O2 and -Os, compiled for mips64-none-elf, linked by embld
# with lib/libc and lib/rt built for it, and run on QEMU's big-endian
# malta (qemu-system-mips64). The same script as mips64-exec.sh, which
# holds the not-applicable list and the programs refereed against clang
# (here clang for mips64-unknown-elf).
MIPS64_EXEC_BE=1 exec sh "$(dirname "$0")/mips64-exec.sh"
