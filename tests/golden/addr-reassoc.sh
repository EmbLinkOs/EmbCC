#!/bin/sh
# Two small rewrites that FreeRTOS's task notifications showed.
#
# `pxTCB->ulNotifiedValue[uxIndexToNotify] |= ulValue` is an address
# `(tcb + 0x44) + (i << 2)` used by a load and a store. Built that way the
# constant cannot be the accesses' offset: `adds r0, #0x44; add.w r1, r0,
# r1, lsl #2; ldr r0, [r1]; ...; str r0, [r1]`. Reassociated to
# `(tcb + (i << 2)) + 0x44`, it is: `add.w r1, r0, r1, lsl #2; ldr r0,
# [r1, #0x44]; ...; str r0, [r1, #0x44]` (ra_fold_memoff). On RISC-V and
# AVR, which cannot add a register in an access, one access is enough.
#
# And `configASSERT(uxIndexToNotify < 1)` -- the default array of one --
# was `cmp r7, #1; blo`: unsigned, `x < 1` is `x == 0`, one cbz
# (branch_on_cmp0). tests/exec/addr-reassoc.c runs both on the boards.
set -u
echo "TEST-MARKER addr-reassoc"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/addr-reassoc
rm -rf "$out"; mkdir -p "$out"

cat > "$out/a.c" <<'EOT'
struct tcb { int pad[17]; unsigned notify[4]; };
void orin(struct tcb *t, unsigned i, unsigned v) { t->notify[i] |= v; }
unsigned get(struct tcb *t, unsigned i) { return t->notify[i]; }
extern void fail(void);
void check(unsigned i) { if (!(i < 1u)) fail(); }
EOT
body() {                        # body OBJDUMP FUNC
    awk -v f="<$2>:" '$2 == f { on = 1; next } /^[0-9a-f]+ <.*>:$/ { on = 0 }
                      on && /^ *[0-9a-f]+:/' "$1"
}
n() { body "$1" "$2" | grep -cE "$3" || true; }
fail() { echo "FAIL: $1:"; body "$2" "$3"; exit 1; }

"$EMBCC" --target=thumbv7em-none-eabi -Os -c "$out/a.c" -o "$out/t.o" &&
"$EMBCC" --target=riscv32-unknown-elf -Os -c "$out/a.c" -o "$out/r.o" || {
    echo "FAIL: could not compile"; exit 1; }
"$OD" -d --no-show-raw-insn "$out/t.o" > "$out/t.dis"
"$OD" -d --no-show-raw-insn "$out/r.o" > "$out/r.dis"

[ "$(n "$out/t.dis" orin '\[r[0-9]+, #0x44\]')" = 2 ] &&
[ "$(n "$out/t.dis" orin 'add.*#0x44')" = 0 ] ||
    fail "Thumb: the load and store of t->notify[i] |= v should take #0x44" "$out/t.dis" orin
echo "Thumb: t->notify[i] |= v keeps 0x44 in the load and the store"
[ "$(n "$out/r.dis" get '(68|0x44)\(')" = 1 ] &&
[ "$(n "$out/r.dis" get 'addi.*(68|0x44)$')" = 0 ] ||
    fail "RISC-V: the load of t->notify[i] should take 68" "$out/r.dis" get
echo "RISC-V: t->notify[i] is one lw at 68 from base + 4i"
[ "$(n "$out/t.dis" check 'cbz|cbnz')" = 1 ] &&
[ "$(n "$out/t.dis" check 'cmp')" = 0 ] ||
    fail "Thumb: an unsigned i < 1 should be a cbz/cbnz of i" "$out/t.dis" check
[ "$(n "$out/r.dis" check 'beqz|bnez')" = 1 ] &&
[ "$(n "$out/r.dis" check 'sltiu|sltu')" = 0 ] ||
    fail "RISC-V: an unsigned i < 1 should be a beqz/bnez of i" "$out/r.dis" check
echo "unsigned i < 1: a test of i itself on Thumb and RISC-V"
