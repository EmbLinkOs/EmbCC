#!/bin/sh
# The limits EmbCC's libc publishes match what it does.
#
# MB_LEN_MAX was 1 although the library's multibyte encoding is UTF-8,
# so `char buf[MB_LEN_MAX]` given to wcrtomb was overrun by the first
# character past U+007F; MB_CUR_MAX was not defined at all; and C's
# <stddef.h> did not declare max_align_t, which C11 requires.
#
# Run on Linux under QEMU, against build/libc/linux-*.
set -u
echo "TEST-MARKER libc-limits"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/libc-limits
rm -rf "${out:?}"; mkdir -p "$out"

cat > "$out/t.c" <<'SRC'
#include <limits.h>
#include <stdlib.h>
#include <stddef.h>
#include <wchar.h>
#include <string.h>
struct guard { char buf[MB_LEN_MAX]; char after[8]; };
int main(void)
{
    struct guard g;
    mbstate_t st;
    memset(&g, 0x55, sizeof g);
    memset(&st, 0, sizeof st);
    if (MB_LEN_MAX < 4 || MB_CUR_MAX != 4 || MB_CUR_MAX > MB_LEN_MAX) return 1;
    size_t n = wcrtomb(g.buf, (wchar_t)0x1F600, &st);   /* four bytes */
    if (n != 4 || (unsigned char)g.buf[0] != 0xF0) return 2;
    for (int i = 0; i < 8; i++)
        if (g.after[i] != 0x55) return 3;                /* nothing past */
    if (_Alignof(max_align_t) < _Alignof(long double) ||
        _Alignof(max_align_t) < _Alignof(long long)) return 4;
    return 42;
}
SRC

A64LD=${EMBCC_AARCH64_LD:-aarch64-elf-ld}
A64GCC=$(dirname "$(aarch64-elf-gcc -print-libgcc-file-name 2>/dev/null)")

ran=0
for arch in x86_64 aarch64; do
    LIBDIR=$EMBCC_ROOT/build/libc/linux-$arch
    [ -f "$LIBDIR/libc.a" ] || { echo "skip $arch: no $LIBDIR/libc.a"; continue; }
    if [ $arch = aarch64 ] && { ! command -v "$A64LD" > /dev/null 2>&1 ||
                                [ ! -f "$A64GCC/libgcc.a" ]; }; then
        echo "skip $arch: no $A64LD and libgcc to link with"; continue
    fi
    "$EMBCC_ROOT/tests/harness/linux/run.sh" $arch --check > /dev/null 2>&1 || {
        echo "skip $arch: no kernel for tests/harness/linux"; continue; }
    for O in -O0 -O2; do
        "$EMBCC" --target=$arch-linux-gnu $O -c "$out/t.c" -o "$out/t.o" \
            > "$out/cc.log" 2>&1 || {
            echo "FAIL $arch $O: did not compile"; cat "$out/cc.log"; exit 1; }
        if [ $arch = x86_64 ]; then
            "$EMBCC_ROOT/embld" -o "$out/t.bin" "$LIBDIR/crt1.o" "$out/t.o" \
                "$LIBDIR/libc.a" > "$out/ld.log" 2>&1
        else
            # embld writes no AArch64 executable; binary128 arithmetic
            # comes from libgcc, after libc.a (as tests/golden/linux.sh).
            "$A64LD" -static -T "$EMBCC_ROOT/lib/libc/os/linux/link.ld" \
                -o "$out/t.bin" "$LIBDIR/crt1.o" "$out/t.o" \
                "$LIBDIR/libc.a" -L"$A64GCC" -lgcc > "$out/ld.log" 2>&1
        fi || { echo "FAIL $arch $O: did not link"; cat "$out/ld.log"; exit 1; }
        "$EMBCC_ROOT/tests/harness/linux/run.sh" $arch "$out/t.bin" \
            > "$out/run.log" 2>&1
        rc=$?
        [ "$rc" = 42 ] || {
            echo "FAIL $arch $O: exit $rc (42 expected)"; cat "$out/run.log"; exit 1; }
        ran=$((ran + 1))
    done
done
echo "libc-limits: $ran runs, MB_LEN_MAX, MB_CUR_MAX and max_align_t agree with the library"
