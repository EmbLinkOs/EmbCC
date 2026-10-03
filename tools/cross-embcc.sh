#!/bin/sh
# cross-embcc.sh TRIPLE OUT -- EmbCC, built by EmbCC alone, for another OS.
#
# Compiles every source of the compiler with ./embcc --target=TRIPLE,
# against the ISO C host layer (PLATFORM=iso, so the OS needs nothing but a
# C library), and links the result with ./embld, the OS's startup object
# and its C library. Nothing from GCC, Clang or binutils takes part. This
# is how a hobby OS with no compiler gets one (docs/internals/porting.md).
#
# For the Linux triples the startup object and the libraries are EmbCC's
# own (make libc-linux-x86_64 / libc-linux-aarch64). For any other OS, say
# where yours are:
#
#   CROSS_CFLAGS   compile flags: the OS's headers, e.g.
#                  "-O1 -nostdinc -isystem /path/to/os/include"
#   CROSS_CRT      its startup object (crt0.o, crt1.o, ...)
#   CROSS_LIBS     its C library and anything it needs, in link order
#   CROSS_LDFLAGS  embld options: entry point and load address, as its
#                  program loader expects (-e _start -Ttext 0x400000)
#   CROSS_OBJ      where the objects go (build/cross-TRIPLE)
set -eu
[ $# -eq 2 ] || { echo "usage: cross-embcc.sh TRIPLE OUT" >&2; exit 2; }
T=$1; OUT=$2
E=${EMBCC:-./embcc}; L=${EMBLD:-./embld}
OBJ=${CROSS_OBJ:-build/cross-$T}
CFLAGS=${CROSS_CFLAGS:--O1}
mkdir -p "$OBJ"
rm -f "$OBJ"/*.o

# The compiler's own sources, as the Makefile lists them, with the ISO
# host layer in place of the POSIX one.
SRCS=$(make -pn PLATFORM=iso 2>/dev/null | sed -n 's/^SRCS := //p' | head -1)
[ -n "$SRCS" ] || { echo "cross-embcc: cannot read SRCS from the Makefile" >&2; exit 1; }
n=0
for s in $SRCS; do
    o=$OBJ/$(echo "$s" | sed 's|^src/||; s|/|_|g; s|\.c$|.o|')
    # shellcheck disable=SC2086
    "$E" --target="$T" $CFLAGS -c "$s" -o "$o" || {
        echo "cross-embcc: $s does not compile for $T" >&2; exit 1; }
    n=$((n + 1))
done
# shellcheck disable=SC2086
"$E" --target="$T" $CFLAGS -DEMBDBG_NO_MAIN -c tools/embdbg/embdbg.c \
    -o "$OBJ/embdbg_core.o"

case $T in
    x86_64-linux-gnu)  d=build/libc/linux-x86_64 ;;
    aarch64-linux-gnu) d=build/libc/linux-aarch64 ;;
    *)                 d= ;;
esac
CRT=${CROSS_CRT:-${d:+$d/crt1.o}}
LIBS=${CROSS_LIBS:-${d:+$d/libc.a $d/librt.a}}
[ -n "$CRT" ] && [ -n "$LIBS" ] || {
    echo "cross-embcc: no C library known for $T: set CROSS_CRT and CROSS_LIBS" >&2
    exit 1; }
# shellcheck disable=SC2086
"$L" ${CROSS_LDFLAGS:-} $CRT "$OBJ"/*.o $LIBS -o "$OUT" || {
    echo "cross-embcc: the link failed" >&2; exit 1; }
echo "cross-embcc: $OUT, from $n sources compiled by EmbCC for $T"
