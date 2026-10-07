#!/bin/sh
# Run one TriCore harness image on QEMU's tricore_testboard and pass its
# output through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# -cpu tc27x: a TriCore 1.6.1 core (the board's default, tc1796, is 1.3
# and has no DIV). -kernel loads the ELF and starts it at its entry. The
# board has no serial port: putc.so, a TCG plugin, prints the bytes the
# harness stores to its UART word (putc.c); EMBCC_TRICORE_PLUGIN says
# where it was built, beside this script by default. The image ends by
# printing `==EXIT n ==` and writing n to the test device, which ends
# QEMU; qrun.sh --until stops at the WHOLE sentinel in any case, and its
# timeout bounds an image that never prints it -- a trap or a wild jump
# runs on through unmapped memory, which reads as zeros, which are NOPs.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_TRICORE:-qemu-system-tricore}
PLUG=${EMBCC_TRICORE_PLUGIN:-$here/putc.so}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" --until "==EXIT [0-9]* ==" \
    "$QEMU" -M tricore_testboard -cpu tc27x -display none -monitor none \
    -plugin "$PLUG" -kernel "$1" 2>/dev/null
exit 0
