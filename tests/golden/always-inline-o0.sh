#!/bin/sh
# always_inline is inlined at -O0, as GCC and clang inline it at every
# level; nothing else is. CMSIS writes its core-register accessors
# (__get_PRIMASK, __enable_irq, ...) as __STATIC_FORCEINLINE functions
# around one instruction, and a debug build of a firmware calls them in
# its critical sections: EmbCC compiled each into a call.
#
#   - on x86-64, Cortex-M, RV32 and AVR, at -O0 and -O0 -g, a caller of an
#     always_inline function holds its body and no call to it, and still
#     calls the plain function beside it;
#   - on the Cortex-M3 board, PRIMASK accessors written the CMSIS way,
#     with inline asm and output operands, run inlined at -O0 and read back
#     what they wrote.
set -u
echo "TEST-MARKER always-inline-o0"
EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=tests/golden/out/always-inline-o0
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
# body OBJ FN: FN's disassembly with its relocations
body() {
    "$OBJDUMP" -dr "$1" | sed -n "/<$2>:/,/^\$/p" | sed 1d
}
cat > "$out/a.c" <<'EOF'
__attribute__((always_inline)) static inline int twice(int x) { return x * 2; }
int plain(int x) { return x + 1; }
int caller(int x) { return twice(x) + plain(x); }
EOF
for T in x86_64-elf thumbv7m-none-eabi riscv32-unknown-elf avr-none-elf; do
    for g in "" -g; do
        "$EMBCC" --target=$T -O0 $g -c "$out/a.c" -o "$out/a-$T.o" ||
            fail "a.c for $T at -O0 $g"
        body "$out/a-$T.o" caller > "$out/caller-$T.txt"
        grep -q "twice" "$out/caller-$T.txt" &&
            fail "$T -O0 $g: caller calls the always_inline twice"
        grep -q "plain" "$out/caller-$T.txt" ||
            fail "$T -O0 $g: caller no longer calls plain, which is not always_inline"
    done
done
echo "at -O0 an always_inline callee is inlined and a plain one is called"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
T=thumbv7m-none-eabi
if command -v "$QEMU" >/dev/null 2>&1; then
    d=$out/m3
    mkdir -p "$d"
    cat > "$d/prog.c" <<'EOF'
void putn(long v);
void puts_(const char *s);
#define __STATIC_FORCEINLINE __attribute__((always_inline)) static inline
__STATIC_FORCEINLINE unsigned get_primask(void)
{
    unsigned r;
    __asm__ volatile("mrs %0, primask" : "=r"(r));
    return r;
}
__STATIC_FORCEINLINE void set_primask(unsigned v)
{
    __asm__ volatile("msr primask, %0" : : "r"(v) : "memory");
}
int main(void)
{
    set_primask(1);
    unsigned a = get_primask();
    set_primask(0);
    unsigned b = get_primask();
    puts_("primask "); putn((long)a); puts_(" "); putn((long)b); puts_("\n");
    return 42;
}
EOF
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c tests/harness/thumb/$f.c -o "$d/$f.o" || fail "$f.c"
    done
    "$EMBCC" --target=$T -O0 -c "$d/prog.c" -o "$d/prog.o" || fail "prog.c at -O0"
    body "$d/prog.o" main | grep -q "[gs]et_primask" &&
        fail "main calls a PRIMASK accessor at -O0: $(body "$d/prog.o" main | grep _primask)"
    body "$d/prog.o" main | grep -q "mrs" ||
        fail "main holds no mrs at -O0"
    EMBCC_THUMB_HARNESS=$d sh tests/harness/thumb/link.sh "$d/prog.elf" "$d/prog.o" \
        > "$d/link.log" 2>&1 || fail "the M3 image does not link: $(head -3 "$d/link.log")"
    sh tests/harness/qrun.sh 10 "$QEMU" -M lm3s6965evb -cpu cortex-m3 -nographic \
        -kernel "$d/prog.elf" > "$d/console.log" 2>&1
    tr -d '\r' < "$d/console.log" | grep -q "^primask 1  *0 *$" ||
        fail "the inlined accessors did not read back 1 then 0: $(head -3 "$d/console.log")"
    echo "CMSIS-style PRIMASK accessors run inlined at -O0 on the Cortex-M3"
else
    echo "SKIP: the board half ($QEMU not found)"
fi
echo "ok always-inline-o0"
