#!/bin/sh
# A member read out of a struct a call returns -- an RTOS's
# `emb_time_now().ticks` -- is an aligned load: the struct is in a slot of
# the caller's own, aligned as its type (irgen.c, lv_natural). It was a
# byte at a time on every strict-alignment target, eight byte loads and
# seven shifts and ors for one 64-bit tick count. A packed struct's
# member is still read a byte at a time (tests/exec/struct-result-members.c
# runs both on the boards).
set -u
echo "TEST-MARKER member-of-result"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/member-of-result
rm -rf "$out"; mkdir -p "$out"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
cat > "$out/m.c" <<'EOC'
typedef unsigned long long u64;
typedef struct { u64 ticks; } tm;
typedef struct __attribute__((packed)) { char c; u64 d; } pk;
tm now(void);
pk pnow(void);
u64 f(void) { return now().ticks + 5; }
u64 g(void) { return pnow().d; }
EOC
fail=0
# Xtensa: -S is bytes; L8UI is op0 2 with r 0 (the second byte's high
# nibble), which no other load has
"$EMBCC" --target=xtensa-esp32-elf -O2 -ffunction-sections -S "$out/m.c" -o "$out/x.s" || exit 1
for fn in f g; do
    sed -n "/^$fn:/,/\\.size/p" "$out/x.s" |
        grep -cE '\.byte[[:space:]]+0x[0-9a-f]2,0x0[0-9a-f],' > "$out/x-$fn.n"
done
[ "$(cat "$out/x-f.n")" = 0 ] || {
    echo "xtensa: now().ticks is read with $(cat "$out/x-f.n") byte loads"; fail=1; }
[ "$(cat "$out/x-g.n")" -ge 8 ] || {
    echo "xtensa: the packed member is not read a byte at a time"; fail=1; }
if command -v "$OD" >/dev/null 2>&1; then
    # the unaligned forms: ARMv6-M's byte loads, MIPS's lwl/lwr (or bytes)
    for t in 'thumbv6m-none-eabi:ldrb' 'mipsel-none-elf:lbu|lwl|lwr'; do
        tg=${t%%:*}; b=${t#*:}
        "$EMBCC" --target=$tg -O2 -ffunction-sections -c "$out/m.c" -o "$out/$tg.o" || exit 1
        "$OD" -d "$out/$tg.o" > "$out/$tg.dis"
        nf=$(sed -n '/<f>:/,/^$/p' "$out/$tg.dis" | grep -cE "[[:space:]]($b)[[:space:]]")
        ng=$(sed -n '/<g>:/,/^$/p' "$out/$tg.dis" | grep -cE "[[:space:]]($b)[[:space:]]")
        [ "$nf" = 0 ] || { echo "$tg: now().ticks is read with $nf unaligned loads ($b)"; fail=1; }
        [ "$ng" -ge 2 ] || { echo "$tg: the packed member is not read unaligned ($ng)"; fail=1; }
    done
fi
[ "$fail" = 0 ] || exit 1
echo "member-of-result: a returned struct's member is one aligned load on Xtensa, ARMv6-M and MIPS; a packed one's still bytes"
