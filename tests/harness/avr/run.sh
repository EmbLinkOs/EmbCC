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
# QEMU's own stderr is KEPT, not discarded. It refuses an AVR image whose
# ELF entry point is not 0x0000 and says so in one clear line -- and that
# line was invisible for an afternoon because this script sent it to
# /dev/null, so a rejected image looked exactly like a program that printed
# nothing. Any diagnostic here is worth more than the tidier output.
"$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-10}" "$QEMU" \
    -M uno -nographic -bios "$1"
exit 0
