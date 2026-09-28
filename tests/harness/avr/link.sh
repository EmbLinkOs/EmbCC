#!/bin/sh
# Link one AVR harness image: the startup, the UART output and the program
# under test, into a firmware layout QEMU's -bios accepts.
#
#   usage: link.sh OUT.elf PROGRAM.o...
#
# Flash at 0, SRAM at 0x100 -- the ATmega328P's SRAM begins there, with
# the 256 bytes below it being the register file and I/O space. -Tdata is
# what makes embld store the writable segment after the text in flash and
# address it in RAM, and provide the __data_load/__bss_start brackets
# boot.S copies and zeroes with.
#
# On this target the writable segment carries .rodata as well, because no
# instruction can read flash where it lies (see boot.S).
#
# boot.o comes from EmbCC's own assembler now (embcc --target=avr -c boot.S),
# not from llvm-mc -- this target needs no other toolchain to produce a
# running image.
#
# EMBCC_AVR_HARNESS says where boot.o and io.o were built; it defaults to
# beside this script. A test that runs CONCURRENTLY with others must set
# it to its own output directory.
set -eu
out=$1; shift
here=$(dirname "$0")
H=${EMBCC_AVR_HARNESS:-$here}
# EVERY rt*.o the harness directory holds, and no more. lib/rt/avr.c is the
# 32-bit multiply/divide/remainder and lib/rt/avr64.c the 64-bit ones, in
# SEPARATE objects so a program pays only for what it calls: with both in one
# object an image that never writes `long long` still carried the 64-bit
# routines, and at -O0 that took this target's own execution test to 34718
# bytes on a part with 32768 of flash. It failed as `CALL 0x8770` -- a call
# past the end of memory -- which is what a runtime library being one
# routine per object exists to prevent.
#
# -e __vectors, not -e reset: the entry must be address 0, because QEMU
# refuses an AVR image whose entry_point is anything else and the silicon
# fetches its first instruction from 0 in any case. __vectors is the
# interrupt vector table, whose first entry jumps to reset.
rts=
for f in "$H"/rt*.o; do
    [ -f "$f" ] && rts="$rts $f"
done
# shellcheck disable=SC2086
"${EMBLD:-./embld}" -e __vectors -Ttext 0x0 -Tdata 0x100 \
    "$H/boot.o" "$H/io.o" $rts "$@" -o "$out"
