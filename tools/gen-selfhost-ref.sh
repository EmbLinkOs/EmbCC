#!/bin/sh
# Generate the self-hosting reference objects (ref/*.o) and relink stage1
# (embcc.elf) from them, for the on-OS fixed point (docs/SELFHOST_ONOS.md).
# The references are the gcc-built embcc's output for EmbCC's own sources;
# STT_FILE is the basename, so they match what the OS embcc produces.
set -eu
. "$(dirname "$0")/hostpaths.sh"
NEWLIB_INC=${NEWLIB_INC:-$X86_NEWLIB/include}
OS_BUILD=${OS_BUILD:-$MYOS_BUILD}
LIBC=${LIBC:-$X86_NEWLIB/lib/libc.a}
INCS="-I include -I $NEWLIB_INC"
# The Makefile's SRCS, not a hand-kept list (which went stale when the tree
# grew src/arch/); objects are named by path, since src/arch/x86_64/codegen.c
# and src/arch/aarch64/codegen.c share a basename.
SRCS=$(make -pn 2>/dev/null | sed -n 's/^SRCS := //p' | head -1)
[ -n "$SRCS" ] || { echo "gen-selfhost-ref: cannot read SRCS from the Makefile" >&2; exit 1; }
rm -rf ref; mkdir -p ref
for f in $SRCS; do
    ./embcc -c "$f" $INCS -o "ref/$(echo "${f#src/}" | tr / _ | sed 's/\.c$//').o"
done
echo "wrote $(ls ref/*.o | wc -l) reference objects"
./embld -o "$OS_BUILD/embcc.elf" "$OS_BUILD/crt0.o" "$OS_BUILD/syscalls.o" \
    ref/*.o "$LIBC"
echo "relinked stage1 -> $OS_BUILD/embcc.elf ($(wc -c < "$OS_BUILD/embcc.elf" | tr -d " ") bytes)"
