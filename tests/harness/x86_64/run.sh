#!/bin/sh
# Boot one x86-64 harness image. Its stdout is the guest's, and its exit status
# is the guest's exit code — read from the @@EMBCC-EXIT n@@ marker sys.c
# prints, since isa-debug-exit cannot carry codes above 127.
#
#   usage: run.sh IMAGE.elf       -> guest code; 124 timeout; 125 crashed/reset
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_X86:-qemu-system-x86_64}
out=$("$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-20}" "$QEMU" -cpu max -m 128M \
          -display none -no-reboot -monitor none -serial none -debugcon stdio \
          -device isa-debug-exit,iobase=0xf4,iosize=0x04 -kernel "$1" 2>/dev/null)
qs=$?
printf '%s\n' "$out" | sed '/^@@EMBCC-EXIT [0-9]*@@$/d'
[ "$qs" -eq 137 ] && exit 124
code=$(printf '%s\n' "$out" | sed -n 's/^@@EMBCC-EXIT \([0-9]*\)@@$/\1/p' | tail -1)
[ -n "$code" ] || exit 125
exit "$code"
