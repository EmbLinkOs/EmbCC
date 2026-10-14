#!/bin/sh
# Build the compiler runtime (lib/rt) for ONE embedded target, as an archive.
#
#   usage: tools/build-rt.sh TRIPLE OUTDIR    -> OUTDIR/librt.a
#          TRIPLE may be riscv32-unknown-elf/ilp32f and the like (below)
#
# Every lib/rt/*.c is compiled for every target. A file that does not apply
# compiles to an empty object -- lib/rt/avrfp*.c everywhere but AVR,
# lib/rt/softfp.c on a machine with hardware float, lib/rt/int128.c where
# __int128 does not exist -- so a REFUSAL here is a real error and stops the
# build. There is no list of files to skip, because a list like that is how a
# target ends up without a routine its backend calls.
#
# It is an ARCHIVE, not a directory of objects, because that is what makes a
# program pay only for what it calls: the linker pulls a member when something
# references a symbol it defines, and not otherwise. On a 32 KB part that is
# the difference between a float program fitting and not -- software binary32
# alone is 40 KB of AVR text at -O0.
#
# -Os: this is firmware, and the runtime is linked into every image.
set -eu
triple=$1
out=$2
# A RISC-V hardware-float ABI's library is named TRIPLE/ABI
# (riscv32-unknown-elf/ilp32f): built with that -mabi and the -march it
# needs, into the directory the driver looks in for it -- objects of two
# float ABIs do not link (src/driver/main.c lib_triple).
flags=
case $triple in
    */*) abi=${triple#*/}; triple=${triple%%/*}
         case $abi in
             ilp32f) flags="-march=rv32imafc -mabi=ilp32f" ;;
             ilp32d) flags="-march=rv32imafdc -mabi=ilp32d" ;;
             lp64f)  flags="-march=rv64imafc -mabi=lp64f" ;;
             lp64d)  flags="-march=rv64imafdc -mabi=lp64d" ;;
             *) echo "$0: no library variant '$abi' for $triple" >&2; exit 1 ;;
         esac ;;
esac
here=$(cd "$(dirname "$0")/.." && pwd)
EMBCC=${EMBCC:-$here/embcc}
# EmbCC's own archiver, built beside the compiler (make embar), so a host
# with no binutils or LLVM can package the runtime; EMBCC_AR overrides it.
AR=${EMBCC_AR:-./embar}
[ -x "$AR" ] || command -v "$AR" >/dev/null 2>&1 || AR=llvm-ar
command -v "$AR" >/dev/null 2>&1 || [ -x "$AR" ] || AR=ar

rm -rf "$out/rt"
mkdir -p "$out/rt"
for f in "$here"/lib/rt/*.c; do
    b=$(basename "$f" .c)
    # shellcheck disable=SC2086
    "$EMBCC" --target="$triple" $flags -fenum-size-neutral -Os -c "$f" -o "$out/rt/$b.o" || {
        echo "build-rt: lib/rt/$b.c does not compile for $triple" >&2
        exit 1; }
done
rm -f "$out/librt.a"
"$AR" rcs "$out/librt.a" "$out"/rt/*.o

# AVR: the startup and the linker script of each part -mmcu= takes
# (src/arch/avr/options.c), which the driver links when -mmcu= names it:
# crt<part>.o from lib/avr/crt.S, and <part>.ld from lib/avr/avr5.ld with
# the part's memories, read from <avr/io.h> for that part.
if [ "$triple" = avr ]; then
    for part in atmega328p atmega328 atmega168p atmega168; do
        "$EMBCC" --target=avr -mmcu=$part -c "$here/lib/avr/crt.S" \
            -o "$out/crt$part.o" || {
            echo "build-rt: lib/avr/crt.S does not assemble for $part" >&2
            exit 1; }
        echo '#include <avr/io.h>' > "$out/rt/io-$part.c"
        "$EMBCC" --target=avr -mmcu=$part -E -dM "$out/rt/io-$part.c" \
            > "$out/rt/io-$part.h" || exit 1
        val() { awk -v n="$1" '$1 == "#define" && $2 == n { print $3 }' "$out/rt/io-$part.h"; }
        fe=$(val FLASHEND) rs=$(val RAMSTART) re=$(val RAMEND) ee=$(val E2END)
        [ -n "$fe" ] && [ -n "$rs" ] && [ -n "$re" ] && [ -n "$ee" ] || {
            echo "build-rt: <avr/io.h> has no memories for $part" >&2; exit 1; }
        sed -e "s/@FLASH@/$((fe + 1))/" -e "s/@RAMSTART@/$((rs))/" \
            -e "s/@RAM@/$((re + 1 - rs))/" -e "s/@EEPROM@/$((ee + 1))/" \
            "$here/lib/avr/avr5.ld" > "$out/$part.ld"
        rm -f "$out/rt/io-$part.c" "$out/rt/io-$part.h"
    done
    # ...and the AVR library, libc.a: lib/avr's avr-libc functions (the
    # string and _P functions, sprintf and its _P forms, the EEPROM), one
    # object per function -- each source compiled once per EMB_AVR_ name
    # it guards -- so that an image carries only what it calls. The same
    # registers on every part, so built once.
    rm -rf "$out/c"
    mkdir -p "$out/c"
    for f in "$here"/lib/avr/*.c; do
        b=$(basename "$f" .c)
        for n in $(sed -n 's/^#ifdef \(EMB_AVR_[A-Z0-9_]*\).*/\1/p' "$f"); do
            "$EMBCC" --target=avr -mmcu=atmega328p -Os -Wall -Wextra -Werror \
                -D$n -c "$f" -o "$out/c/$b-$n.o" || {
                echo "build-rt: lib/avr/$b.c ($n) does not compile" >&2
                exit 1; }
        done
    done
    rm -f "$out/libc.a"
    "$AR" rcs "$out/libc.a" "$out"/c/*.o
fi
