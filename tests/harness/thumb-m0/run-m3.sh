#!/bin/sh
# The reference run for the ARMv6-M exec suite: an image built from the
# same harness with -DHARNESS_LM3S (and -DSRAM_TOP=0x20010000) on QEMU's
# Cortex-M3 board, exit status as run.sh reports it.
#
#   usage: run-m3.sh IMAGE.elf
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
# --until is the WHOLE sentinel, status and closing ==: the UART writes a
# byte at a time, and stopping at '==EXIT ' killed QEMU before the status
# now and then -- an empty status, 124, for a program that had passed
# (thumbv8mbase-exec on CI, three of 1256 runs; tests/harness/mips/run.sh)
o=$("$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until '==EXIT -*[0-9][0-9]*==' \
    "$QEMU" -M lm3s6965evb -cpu cortex-m3 -nographic -kernel "$1" 2>/dev/null)
printf '%s\n' "$o" | sed '/^==EXIT /d'
code=$(printf '%s\n' "$o" | sed -n 's/^==EXIT \(-*[0-9]*\)==.*/\1/p' | tail -1)
[ -n "$code" ] || exit 124
exit $((code & 255))
