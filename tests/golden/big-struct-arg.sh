#!/bin/sh
# A struct passed by value that is bigger than an immediate offset reaches
# is copied to the outgoing area by a loop, in time, on every target that
# passes it in memory.
#
# GCC's torture suite found the ARMv7-M backend stopping at the first
# word past 4 KB ("internal error: thumb: a 4-byte load at offset 4096
# from r2 is out of reach": execute/pr20621-1.c, pr23135.c, pr28982b.c)
# and, on ARMv6-M, compile/20050622-1.c's 72 MB struct a compile that did
# not finish: both copied the struct word by word in straight-line code.
# So each shape must compile in 30 s into an object of a few KB -- a copy
# unrolled over 64 KB is more than that by itself -- and the argument
# after the struct, as far from the sp, must be stored too.
# tests/exec/far-offsets.c runs a 4 KB one on the boards.
set -u
echo "TEST-MARKER big-struct-arg"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/big-struct-arg
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/arg.c" <<'EOT'
struct S { char buf[72 * 1024 * 1024]; };
extern void bar(struct S);
struct S s;
int foo(void) { bar(s); return 0; }

struct W { int w[0x4000]; };
extern void baz(struct W, int, long long);
struct W gw;
int foo2(int x) { baz(gw, x, 3); return 0; }

struct O { char c[4099]; };
extern void qux(int, struct O, int);
int foo3(struct O *p) { qux(1, *p, 2); return 0; }
EOT

limit() { perl -e 'alarm 30; exec @ARGV' "$@"; }
n=0
for t in thumbv7em-none-eabi thumbv6m-none-eabi thumbv8m.base-none-eabi \
         thumbv7em-none-eabihf armv7a-none-eabi riscv32-unknown-elf \
         riscv64-unknown-elf x86_64-elf aarch64-elf loongarch64-unknown-elf \
         tricore-none-elf powerpc-none-eabi sparc-none-elf; do
    for o in -O0 -O2; do
        obj=$out/$t$o.o
        limit "$EMBCC" --target=$t $o -c "$out/arg.c" -o "$obj" ||
            fail "$t $o: a big struct argument did not compile, or not in 30 s"
        sz=$(wc -c < "$obj" | tr -d ' ')
        [ "$sz" -lt 16384 ] ||
            fail "$t $o: a $sz-byte object -- the copy is not a loop"
        n=$((n + 1))
    done
done
echo "big-struct-arg: $n compiles, each a loop in an object under 16 KB"
