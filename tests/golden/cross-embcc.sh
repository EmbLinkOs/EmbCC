#!/bin/sh
# EmbCC, built by EmbCC alone for another OS, compiles there.
#
# tools/cross-embcc.sh compiles every source of the compiler with ./embcc
# (PLATFORM=iso, so the OS needs only a C library) and links it with
# ./embld and EmbCC's own Linux C library: no GCC, Clang or binutils takes
# part. The result is booted on a real Linux kernel under QEMU as its init,
# with the headers beside it, and compiles a program that includes
# <stdio.h> -- and must emit exactly the assembly this host's EmbCC emits.
# That is the path a hobby OS with no compiler takes to get one
# (docs/internals/porting.md).
set -u
echo "TEST-MARKER cross-embcc"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=$PWD/tests/golden/out/cross-embcc-$ARCH
rm -rf "${out:?}"; mkdir -p "$out"

[ -f build/libc/linux-x86_64/libc.a ] || {
    echo "SKIP: no EmbCC C library for Linux (make libc-linux-x86_64)"; exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "SKIP: no Linux kernel or QEMU for x86-64"; exit 0; }

EMBCC="$EMBCC" CROSS_OBJ="$out/obj" sh tools/cross-embcc.sh x86_64-linux-gnu \
    "$out/embcc-linux" > "$out/build.log" 2>&1 || {
    echo "FAIL: EmbCC could not build itself for Linux:"; tail -5 "$out/build.log"; exit 1; }
echo "EmbCC built itself for x86_64-linux-gnu with EmbCC, embld and its own libc"

R=$out/root
mkdir -p "$R/lib/libc" "$R/src"
cp -R lib/libc/include "$R/lib/libc/include"
cp -R include "$R/include"
cat > "$R/src/t.c" <<'CEOF'
#include <stdio.h>
#define K 6
static const char *names[] = { "alpha", "beta" };
int f(int x) { return x * K + (int)sizeof(FILE *); }
const char *g(int i) { return names[i & 1]; }
CEOF
EMBCC_LINUX_ROOT=$R EMBCC_LINUX_INIT_ARGS="--target=x86_64-elf -O2 -S /src/t.c -o -" \
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/embcc-linux" \
    > "$out/guest.s" 2>&1
rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: the Linux-hosted EmbCC exited $rc:"; head -5 "$out/guest.s"; exit 1; }
"$EMBCC" --target=x86_64-elf -O2 -S "$R/src/t.c" -o "$out/host.s" || exit 1
sed "s|$R/src/t.c|/src/t.c|" "$out/host.s" > "$out/host2.s"
cmp -s "$out/host2.s" "$out/guest.s" || {
    echo "FAIL: the Linux-hosted EmbCC emits different assembly:"
    diff "$out/host2.s" "$out/guest.s" | head -10; exit 1; }
echo "and on a Linux kernel it compiles <stdio.h> code to exactly this host's assembly"
