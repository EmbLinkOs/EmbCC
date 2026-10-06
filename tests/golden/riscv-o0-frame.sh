#!/bin/sh
# RV64 -O0: temporaries share stack slots, as at RV32.
#
# Temporaries with no register share a pool of slots (ra_coalesce_temps):
# two whose lives do not overlap take one. A 64-bit temporary at RV32 is
# a register pair with a slot of its own, and the frame layout read the
# `wide` map to find those -- without asking the XLEN. At RV64 the map
# marks every 64-bit value, so every one got a slot of its own: FreeRTOS's
# xTaskIncrementTick had a 2384-byte -O0 frame at RV64 against 128 at RV32,
# and the timer task overflowed its stack. An RV64 frame here must be at
# most twice the RV32 one (its slots are twice as wide) and a little.
# EMBCC_O0_NORA=1, the -O0 that allocates no register, so that every
# temporary needs a slot and the pool is what is measured.
set -u
echo "TEST-MARKER riscv-o0-frame"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/riscv-o0-frame
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
cat > "$out/f.c" <<'EOF'
struct item { struct item *next, *prev; long v; unsigned flags; };
long walk(struct item *p, int n, long k)
{
    long s = 0, t = 1;
    for (int i = 0; i < n; i++) {
        s += p->v * 3 + i;
        t = t * k + (long)(p->flags & 7u);
        if (p->flags & 8u)
            s -= t >> 2;
        p = p->next;
    }
    return s + t;
}
EOF
for xl in 32 64; do
    EMBCC_O0_NORA=1 "$EMBCC" --target=riscv$xl-unknown-elf -O0 \
        -fstack-usage -c "$out/f.c" -o "$out/f$xl.o" || fail "rv$xl: compile"
done
a=$(cut -f2 "$out/f32.su"); b=$(cut -f2 "$out/f64.su")
[ -n "$a" ] && [ -n "$b" ] || fail "no stack usage"
[ "$b" -le $((2 * a + 32)) ] ||
    fail "RV64's -O0 frame is $b bytes against RV32's $a: its temporaries do not share slots"
echo "riscv-o0-frame: -O0 frames of $a (RV32) and $b (RV64) bytes"
