#!/bin/sh
# RISC-V without the A extension: -march=rv32imc, an ESP32-C3's ISA.
#
# EmbCC refused any -march= without the A ("its atomics are lr/sc and the
# amo instructions, and it has no __atomic_* library path"), even for code
# that uses no atomics. Without A there is no atomic read-modify-write
# instruction, so GCC and clang call libatomic's sized routines for every
# exchange, compare-exchange and fetch-and-op, and keep aligned loads and
# stores inline; EmbCC now does the same (target_atomic_rmw_libcall), with
# lib/rt/atomic-riscv.c's routines, which mask interrupts. And because the
# C library's locks are amo instructions in the ordinary runtime, a part
# without A links its own: build/libc/<triple>/noa (src/driver/main.c,
# lib_triple). tests/golden/exec-boards.sh runs the whole exec corpus so
# (boards rv32noa and rv64noa). This checks:
#  1. -march=rv32imc's atomic macros are clang's: no __riscv_a or
#     __riscv_atomic or __GCC_HAVE_SYNC_COMPARE_AND_SWAP_N, lock-free 1;
#  2. a program that takes the C library's locks and an atomic links from
#     the noa runtime and libc with no A instruction in it -- and the same
#     program linked for rv32imac does have them, so the check sees them
#     (llvm-objdump prints them as <unknown> unless told of A);
#  3. the object's .riscv.attributes name no A either: they said
#     rv32i2p1_m2p0_a2p1_c2p0 for rv32imc, and a linker merging them marked
#     the image as needing the extension.
set -u
echo "TEST-MARKER riscv-noa"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
CLANG=${EMBCC_CLANG:-clang}
out=tests/golden/out/riscv-noa
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
T=--target=riscv32-unknown-elf
pick='LOCK_FREE|SYNC_COMPARE_AND_SWAP|__riscv_a |__riscv_atomic'
"$EMBCC" $T -march=rv32imc -E -dM -x c /dev/null | grep -E "$pick" | sort > "$out/embcc.txt" ||
    fail "-march=rv32imc does not preprocess"
if command -v "$CLANG" >/dev/null 2>&1 &&
   "$CLANG" $T -march=rv32imc -E -dM -x c /dev/null > "$out/clang-all.txt" 2>/dev/null; then
    grep -E "$pick" "$out/clang-all.txt" | sort > "$out/clang.txt"
    grep -v CHAR8_T "$out/embcc.txt" > "$out/embcc2.txt"
    grep -v CHAR8_T "$out/clang.txt" > "$out/clang2.txt"
    diff "$out/clang2.txt" "$out/embcc2.txt" > "$out/macros.diff" ||
        { cat "$out/macros.diff"; fail "-march=rv32imc's atomic macros are not clang's"; }
    echo "riscv-noa: -march=rv32imc's atomic macros are clang's ($(wc -l < "$out/embcc.txt" | tr -d ' '))"
else
    grep -q '__riscv_atomic\|SYNC_COMPARE_AND_SWAP' "$out/embcc.txt" &&
        fail "-march=rv32imc still says it has atomics"
    echo "riscv-noa: (no clang) -march=rv32imc drops the A extension's macros"
fi
for v in noa ""; do
    d=build/libc/riscv32-unknown-elf${v:+/$v}
    [ -f $d/libc.a ] && [ -f $d/librt.a ] || fail "no $d/libc.a or librt.a (make rt-embedded libc-embedded)"
    [ $d/librt.a -nt lib/rt/atomic-riscv.c ] || fail "$d/librt.a is older than lib/rt/atomic-riscv.c (make rt-embedded)"
done
# the archives themselves, object by object (code apart from data): the
# noa runtime and C library hold no A instruction, the ordinary C library
# does (its locks), so the check sees them
A='[[:space:]](lr|sc)\.[wd]|[[:space:]]amo[a-z]+\.[wd]'
for x in 32 64; do
    d=build/libc/riscv$x-unknown-elf
    for f in $d/noa/libc.a $d/noa/librt.a; do
        [ -f $f ] || fail "no $f (make rt-embedded libc-embedded)"
        n=$(llvm-objdump -d --mattr=+a $f | grep -cE "$A")
        [ "$n" = 0 ] || fail "$f: $n A instructions"
    done
    n=$(llvm-objdump -d --mattr=+a $d/libc.a | grep -cE "$A")
    [ "$n" -gt 0 ] || fail "$d/libc.a: no A instruction found, so the check sees nothing"
done
cat > "$out/p.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
_Atomic int hits;
int main(void)
{
    char *p = malloc(16);
    atomic_fetch_add(&hits, 1);
    snprintf(p, 16, "%d", hits);
    puts(p);
    free(p);
    return 0;
}
void _start(void) { main(); for (;;) ; }
EOF
L="--linker-arg=-Ttext=0x80000000 --linker-arg=-Tdata=0x80400000"
# shellcheck disable=SC2086
"$EMBCC" $T -march=rv32imc -O2 $L "$out/p.c" -o "$out/noa.elf" ||
    fail "-march=rv32imc: a program does not compile and link"
n=$(llvm-objdump -d --mattr=+a "$out/noa.elf" | grep -cE "$A")
[ "$n" = 0 ] || fail "-march=rv32imc: $n A instructions in the image"
llvm-objdump -d "$out/noa.elf" | grep -qE 'jalr?[[:space:]].*<__atomic_fetch_add_4>' ||
    fail "-march=rv32imc: the atomic is not a call to __atomic_fetch_add_4"
# shellcheck disable=SC2086
"$EMBCC" $T -O2 $L "$out/p.c" -o "$out/a.elf" || fail "rv32imac: does not link"
n=$(llvm-objdump -d --mattr=+a "$out/a.elf" | grep -cE "$A")
[ "$n" -gt 0 ] || fail "rv32imac: no A instruction found, so the check above sees nothing"
echo "riscv-noa: rv32imc links its own C library and runtime, with no A instruction ($n in the rv32imac image)"
"$EMBCC" $T -march=rv32imc -O2 -c "$out/p.c" -o "$out/noa.o" ||
    fail "-march=rv32imc: p.c does not compile"
arch=$(llvm-readelf -A "$out/noa.o" | sed -n 's/^ *Value: \(rv32.*\)/\1/p')
[ "$arch" = rv32i2p1_m2p0_c2p0 ] ||
    fail "-march=rv32imc: .riscv.attributes name '$arch', not rv32i2p1_m2p0_c2p0"
echo "riscv-noa: rv32imc's objects name $arch in .riscv.attributes"
