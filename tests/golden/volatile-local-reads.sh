#!/bin/sh
# Every read of a volatile LOCAL is a read of its slot.
#
# A volatile local is the one C asks to survive a longjmp with the value
# it was given after the setjmp, and the one a debugger or a signal handler
# may change: each read must load it. The Thumb backends' reload caches
# kept the register a value was last stored from or loaded into, and
# answered a later read of the same slot from it -- a volatile local's
# too. `volatile int changed = 0; if (__builtin_setjmp(b) == 0) {
# changed = 1; ... }` read `changed` after the jump back as `mov r0, r4`:
# the 0 the store had come from, still in callee-saved r4
# (tests/exec/builtin-setjmp.c, ARMv6-M at -O2).
#
# f stores its volatile local, calls out, and reads it twice: the object
# code must load it from the frame twice, on ARMv6-M and ARMv7-M (both
# reload caches) and RV32, at -O1, -O2 and -Os.
set -u
echo "TEST-MARKER volatile-local-reads"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/volatile-local-reads
rm -rf "$out"; mkdir -p "$out"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }

cat > "$out/v.c" <<'CEOF'
void g(void);
int f(int k)
{
    volatile int x = k;
    g();
    return x + x;
}
CEOF
fail=0
for t in thumbv6m-none-eabi thumbv7em-none-eabi riscv32-unknown-elf; do
    for o in -O1 -O2 -Os; do
        "$EMBCC" --target=$t $o -c "$out/v.c" -o "$out/v.o" ||
            { echo "FAIL: $t $o does not compile"; fail=1; continue; }
        # loads from the frame, not the epilogue's restores of the
        # registers it saved (ra, s0..s11 on RISC-V; pop on Thumb)
        n=$("$OD" -d --no-show-raw-insn --disassemble-symbols=f "$out/v.o" |
            grep -E '\sldr\s.*\[sp|\slw\s.*\(sp\)' |
            grep -vcE '\slw\s+(ra|s[0-9]+),')
        if [ "$n" -lt 2 ]; then
            echo "FAIL: $t $o: the volatile local is read $n time(s) from its slot, not 2"
            "$OD" -d --no-show-raw-insn --disassemble-symbols=f "$out/v.o" | sed 's/^/    /'
            fail=1
        fi
    done
done
[ $fail = 0 ] || exit 1
echo "a volatile local is read from its slot at every read: ARMv6-M, ARMv7-M, RV32 at -O1, -O2, -Os"
