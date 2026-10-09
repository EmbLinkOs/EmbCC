#!/bin/sh
# Cortex-M code size: the shapes that cost bytes, pinned.
#
# r9-r12 are high registers, and every instruction naming one is a 32-bit
# encoding where r0-r7 would take the 16-bit form. Where a lowering's
# scratch can be a free low register it now is, and these check that it
# stays so, on tests/exec/size-shapes.c at -Os (whose values exec-boards.sh
# runs on the boards):
#   - stack arguments: no `mov r12, rN; str.w r12, [sp, #n]` -- a value in
#     a register is stored from it, one in memory through a low register;
#   - a composite returned in memory is copied to the caller's buffer
#     through r0-r2: the copy names none of r9-r12;
#   - a parameter that arrives on the stack is copied to its slot through
#     a pushed low register, not r12;
#   - a 64-bit constant that costs more than twelve bytes to build is
#     `ldrd rlo, rhi, [pc, #n]` from a pool after the function, one slot
#     per value, and the ones out of the ldrd's reach are still built
#     (tests/exec/lit64-pool.c, whose values the boards check).
set -u
echo "TEST-MARKER thumb-size"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP: no $OBJDUMP"; exit 0; }
out=tests/golden/out/thumb-size
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
dis() { "$OBJDUMP" -d --no-show-raw-insn --triple=thumbv7em "$1" |
            sed -n "/<$2>:/,/^\$/p"; }

"$EMBCC" --target=thumbv7em-none-eabi -Os -c tests/exec/size-shapes.c \
    -o "$out/s.o" || fail "compile size-shapes.c"
dis "$out/s.o" caller > "$out/caller.dis"
grep -q 'bl.*<many>' "$out/caller.dis" || fail "caller: no call to many()"
if grep -Eq 'str(\.w)?[[:space:]]+(r12|ip), \[sp' "$out/caller.dis"; then
    cat "$out/caller.dis"; fail "caller: a stack argument stored through r12"
fi
for f in mk30 mk20 mk7; do
    dis "$out/s.o" $f > "$out/$f.dis"
    if grep -Eq '(^|[^a-z0-9])(r9|r10|r11|r12|sb|sl|fp|ip)([^a-z0-9]|$)' "$out/$f.dis"; then
        cat "$out/$f.dis"; fail "$f: the returned composite is copied through r9-r12"
    fi
done
for f in many stk7; do
    dis "$out/s.o" $f > "$out/$f.dis"
    if grep -Eq 'ldr(\.w)?[[:space:]]+(r12|ip), \[sp' "$out/$f.dis"; then
        cat "$out/$f.dis"; fail "$f: a stack parameter copied through r12"
    fi
done

"$EMBCC" --target=thumbv7em-none-eabi -Os -c tests/exec/lit64-pool.c \
    -o "$out/p.o" || fail "compile lit64-pool.c"
for f in mix shared poly; do dis "$out/p.o" $f > "$out/$f.dis"; done
lit() { grep -c 'ldrd.*\[pc' "$out/$1.dis"; }
[ "$(lit shared)" -ge 3 ] || { cat "$out/shared.dis"; fail "shared: the repeated constant is not loaded from the pool"; }
[ "$(grep -c '\.word\|ldrd.*\[pc' "$out/shared.dis")" -ge 3 ] || fail "shared: no pool"
[ "$(lit poly)" -ge 4 ] || { cat "$out/poly.dis"; fail "poly: its doubles are not loaded from the pool"; }
[ "$(lit mix)" -ge 20 ] || fail "mix: fewer than 20 constants from the pool"
grep -q 'movt' "$out/mix.dis" || fail "mix: every constant in the pool, though most are out of reach"
# Four 64-bit values in one function (strtol's conv: the accumulator,
# the cutoff, the base widened, and the -1 they start from) took a
# register pair each, r4 to r11, from every 32-bit value that crosses its
# calls -- the string pointer among them, reloaded from the frame on every
# trip of the digit loop. The pair budget (g_t_pair_cap) tries pairs for
# only the one or two most used. conv was 450 bytes with 60 stack
# accesses; with the budget 428 and 35.
"$EMBCC" --target=thumbv7em-none-eabi -Os -Ilib/libc/include \
    -c lib/libc/src/stdlib/strtol.c -o "$out/strtol.o" || fail "compile strtol.c"
dis "$out/strtol.o" conv > "$out/conv.dis"
nsp=$(grep -cE '(ldr|str)[a-z.]*[[:space:]].*\[sp' "$out/conv.dis") || nsp=0
[ "$nsp" -le 40 ] || { fail "strtol's conv makes $nsp stack accesses (the pair budget keeps it near 35)"; }
echo "thumb-size: stack arguments, stack parameters and returned composites use low registers, and a function's 64-bit values leave registers for its other values"
