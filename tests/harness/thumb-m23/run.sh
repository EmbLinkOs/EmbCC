#!/bin/sh
# Run one ARMv8-M Baseline harness image and exit with its status.
#
#   usage: run.sh IMAGE.elf
#
# QEMU models no Cortex-M23, so the image runs on the mps2-an505's
# Cortex-M33, which executes the Baseline subset (boot.c says what that does
# and does not show). The run ends at the image's `==EXIT n==` line
# (../qrun.sh --until), and its status is n; 124 when it never printed one.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
o=$("$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until '==EXIT ' \
    "$QEMU" -M mps2-an505 -cpu cortex-m33 -nographic -kernel "$1" 2>/dev/null)
printf '%s\n' "$o" | sed '/^==EXIT /d'
code=$(printf '%s\n' "$o" | sed -n 's/^==EXIT \(-*[0-9]*\)==.*/\1/p' | tail -1)
[ -n "$code" ] || exit 124
exit $((code & 255))
