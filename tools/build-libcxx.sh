#!/bin/sh
# Build the C++ runtime for ONE embedded target, as an archive: what the
# code the C++ front end emits calls, and nothing above it.
#
#   usage: tools/build-libcxx.sh TRIPLE OUTDIR    -> OUTDIR/libcxx.a
#
# The subset of lib/libcxx that a C++ program built with -fno-exceptions
# -fno-rtti needs -- the embedded C++ an RTOS wrapper is written in:
#
#   new.cc     operator new and delete, over malloc
#   guard.cc   __cxa_guard_acquire/release/abort (function-local statics),
#              __cxa_pure_virtual, __cxa_deleted_virtual
#   atexit.cc  __aeabi_atexit (ARM) and __dso_handle; __cxa_atexit itself is
#              lib/libc's (tools/build-libc.sh), beside atexit and exit
#
# Built with -fno-exceptions -fno-rtti, as the programs are: there is no
# unwinder on these targets, so no type_info, no exception objects and no
# personality routine. Link it before libc.a and librt.a.
#
# -Os: this is firmware, and the runtime is linked into every image.
set -eu
triple=$1
out=$2
here=$(cd "$(dirname "$0")/.." && pwd)
EMBCC=${EMBCC:-$here/embcc}
AR=${EMBCC_AR:-./embar}
[ -x "$AR" ] || command -v "$AR" >/dev/null 2>&1 || AR=llvm-ar
command -v "$AR" >/dev/null 2>&1 || [ -x "$AR" ] || AR=ar

rm -rf "$out/cxx"
mkdir -p "$out/cxx"
for b in new guard atexit; do
    "$EMBCC" --target="$triple" -Os -fno-exceptions -fno-rtti -x c++ \
        -I"$here/lib/libcxx/include" -I"$here/lib/libc/include" \
        -c "$here/lib/libcxx/src/$b.cc" -o "$out/cxx/$b.o" || {
        echo "build-libcxx: lib/libcxx/src/$b.cc does not compile for $triple" >&2
        exit 1; }
done
rm -f "$out/libcxx.a"
"$AR" rcs "$out/libcxx.a" "$out"/cxx/*.o
