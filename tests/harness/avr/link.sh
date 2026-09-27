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
# rt.o is lib/rt/avr.c: multiply, divide and remainder, which this target
# has no instructions for. It goes on every link because the backend emits
# calls to it and a bare part has no library to find them in.
"${EMBLD:-./embld}" -e reset -Ttext 0x0 -Tdata 0x100 \
    "$H/boot.o" "$H/io.o" "$H/rt.o" "$@" -o "$out"
