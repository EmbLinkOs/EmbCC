#!/bin/sh
# Build lib/libc for ONE embedded target, as an archive, on the bare-metal
# backend (lib/libc/os/baremetal/backend.c).
#
#   usage: tools/build-libc.sh TRIPLE OUTDIR    -> OUTDIR/libc.a
#          TRIPLE may be riscv32-unknown-elf/ilp32f and the like (below)
#
# The portable sources are every target's library verbatim; the backend is
# the part with no operating system under it, and it needs nothing from the
# program to link -- write(), sbrk() and the rest are weak, so a program
# that defines write() over its UART gets printf there and one that does
# not still runs. Every file is compiled: one that does not compile for the
# target stops the build, as in tools/build-rt.sh, because a missing member
# is how a program ends up without the routine it calls.
#
# -Os: this is firmware, and the library is linked into every image -- an
# archive, so an image pays only for the members it uses.
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
AR=${EMBCC_AR:-./embar}
[ -x "$AR" ] || command -v "$AR" >/dev/null 2>&1 || AR=llvm-ar
command -v "$AR" >/dev/null 2>&1 || [ -x "$AR" ] || AR=ar

rm -rf "$out/c"
mkdir -p "$out/c"
for f in "$here"/lib/libc/src/*/*.c "$here"/lib/libc/src/math/fdlibm/*.c \
         "$here"/lib/libc/os/baremetal/backend.c; do
    o=$(echo "${f#$here/}" | tr / _ | sed 's/\.c$/.o/')
    # shellcheck disable=SC2086
    "$EMBCC" --target="$triple" $flags -fenum-size-neutral -Os -I"$here/lib/libc/include" \
        -I"$here/lib/libc/src/math" -c "$f" -o "$out/c/$o" || {
        echo "build-libc: ${f#$here/} does not compile for $triple" >&2
        exit 1; }
done
rm -f "$out/libc.a"
"$AR" rcs "$out/libc.a" "$out"/c/*.o
