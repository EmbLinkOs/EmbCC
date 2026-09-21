#!/bin/sh
# Boot one aarch64 harness image on QEMU virt. Its stdout is the guest's, and
# its exit status is the guest's exit code (semihosting carries it directly).
#
#   usage: run.sh IMAGE.elf       -> guest code; 124 timeout
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_AARCH64:-qemu-system-aarch64}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-20}" "$QEMU" -M virt -cpu cortex-a72 \
    -semihosting -nographic -kernel "$1"
qs=$?
[ "$qs" -eq 137 ] && exit 124
exit "$qs"
