#!/bin/sh
# Run one AVR harness image on QEMU's arduino-uno (ATmega328P) and pass
# USART0 through to stdout.
#
#   usage: run.sh IMAGE.elf
#
# The image never exits: a reset handler is the bottom of the call stack
# on a bare part, and this machine has no semihosting call to stop the
# emulator with. So the run is bounded by ../qrun.sh's timeout and judged
# by its OUTPUT -- which is why every harness program ends by printing a
# sentinel the caller looks for. Being killed is the expected end, so the
# status is not the answer and this always exits 0.
set -u
here=$(dirname "$0")
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" "$QEMU" \
    -M uno -nographic -bios "$1" 2>/dev/null
exit 0
