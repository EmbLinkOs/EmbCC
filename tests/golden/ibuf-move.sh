#!/bin/sh
# No optimizer pass reads an instruction through a pointer ib_push gave it
# after a later push.
#
# ib_push grows its buffer with realloc, so such a pointer dangles -- but
# reading it goes wrong only when the later push happens to grow the
# buffer. One in strength reduction crashed the compiler at -Os only once
# fuzzer seeds landed on a growth, and a search found two more (the
# vectorizer's reduction setup, forwarding through a punning union).
# EMBCC_IBUF_MOVE=1 makes every push move the buffer and scribble on the
# old one, so a dangling read or write goes wrong every time. The objects must then be byte-identical to
# the normal ones. -g puts each instruction's line in the object, so a
# line read through a dangling pointer shows too.
#
# The corpus is tests/exec on three targets. Every push copies the whole
# function, so the few files whose functions run to thousands of
# instructions are left out: they would take minutes and exercise the
# same passes.
set -u
echo "TEST-MARKER ibuf-move"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/ibuf-move
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
big=" big-copy complex far-switch fold-float-constants imm-edges mulconst
      packed-wide-bitfields range-check "
n=0
for tO in x86_64-elf:-O2 x86_64-elf:-Os thumbv7m-none-eabi:-Os riscv32-unknown-elf:-Os; do
    t=${tO%%:*} O=${tO#*:}
    for f in tests/exec/*.c; do
        b=$(basename "$f" .c)
        case "$big" in *" $b "*) continue ;; esac
        "$EMBCC" --target=$t $O -g -c "$f" -o "$out/a.o" 2>/dev/null || continue
        EMBCC_IBUF_MOVE=1 "$EMBCC" --target=$t $O -g -c "$f" -o "$out/b.o" \
            2> "$out/b.err" || {
            head -3 "$out/b.err"
            fail "$t $O $f: the compile fails when every push moves the buffer"; }
        cmp -s "$out/a.o" "$out/b.o" ||
            fail "$t $O $f: the object differs when every push moves the buffer"
        n=$((n + 1))
    done
done
[ $n -gt 400 ] || fail "only $n compiles: the corpus did not compile"
echo "ibuf-move: $n compiles are byte-identical when every ib_push moves the buffer"
