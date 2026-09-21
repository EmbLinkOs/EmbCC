#!/bin/sh
# Link one embcc-produced x86-64 object into a Multiboot image the QEMU
# harness can boot, then leave it at $2.
#
#   usage: link.sh [--cxx] -o OUTPUT.elf OBJECT...
#
# As in ../aarch64/link.sh, the harness pieces are built with the cross gcc
# on purpose: they are scaffolding, and only OBJECT came from embcc.
set -eu

# --cxx: a C++ program — link libstdc++ and libsupc++ from the reference
# toolchain (tools/build-ref-gxx.sh), which EmbCC-compiled C++ uses until it
# compiles them itself (D-013).
CXXLIBS=""
if [ "${1:-}" = --cxx ]; then
    shift
    REF="${EMBCC_REF_GXX:-$HOME/cross/gcc-cxx-x86_64-elf}"
    [ -f "$REF/x86_64-elf/lib/libstdc++.a" ] || {
        echo "link.sh: no reference libstdc++ under $REF (tools/build-ref-gxx.sh x86_64-elf)" >&2
        exit 1
    }
    CXXLIBS="-L$REF/x86_64-elf/lib -lstdc++ -lsupc++"
fi
[ "${1:-}" = -o ] && [ $# -ge 3 ] || { echo "usage: link.sh -o OUTPUT.elf OBJECT..." >&2; exit 2; }
out=$2
shift 2
here=$(dirname "$0")
work=${EMBCC_HARNESS_WORK:-$(dirname "$out")}

NEWLIB="${EMBCC_X86_NEWLIB:-$HOME/cross/newlib-c99/x86_64-elf}"
GCC="${EMBCC_X86_GCC:-x86_64-elf-gcc}"
LD="${EMBCC_X86_LD:-x86_64-elf-ld}"
OBJCOPY="${EMBCC_X86_OBJCOPY:-x86_64-elf-objcopy}"

[ -d "$NEWLIB/lib" ] || {
    echo "link.sh: no x86_64 newlib at $NEWLIB" >&2
    echo "link.sh: set EMBCC_X86_NEWLIB to its <triple> directory" >&2
    exit 1
}

for part in start sys crt; do
    src=$here/$part.$( [ "$part" = start ] && echo S || echo c )
    [ "$part" = crt ] && src=$here/../crt.c
    o=$work/harness-x86-$part.o
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ]; then
        $GCC -ffreestanding -mno-red-zone -isystem "$NEWLIB/include" -c "$src" -o "$o"
    fi
done

LIBGCC=$(dirname "$($GCC -print-libgcc-file-name)")
$LD -n -z max-page-size=0x1000 -T "$here/link.ld" -o "$out.64" \
    "$work/harness-x86-start.o" "$@" "$work/harness-x86-sys.o" \
    "$work/harness-x86-crt.o" $CXXLIBS \
    -L"$NEWLIB/lib" -lc -lm -L"$LIBGCC" -lgcc 2>&1 \
    | grep -v 'LOAD segment with RWX permissions' >&2 || true
[ -f "$out.64" ]
# QEMU's Multiboot loader accepts ELF32 only. Every address is below 4 GiB,
# so re-labelling the linked image is exact; the 64-bit code is just bytes.
$OBJCOPY -I elf64-x86-64 -O elf32-i386 "$out.64" "$out"
# EMBCC_HARNESS_KEEP64=1 keeps the 64-bit image beside it: the one a debugger
# loads for symbols and DWARF (tests/golden/debug-live.sh).
[ "${EMBCC_HARNESS_KEEP64:-}" = 1 ] || rm -f "$out.64"
