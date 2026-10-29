#!/bin/sh
# GCC's __builtin___clear_cache(begin, end): what it is lowered onto.
# tests/exec/builtin-clear-cache.c runs a function written into RAM, then
# rewritten and cleared, on every board.
#
#  1. A non-pointer is refused by name; __has_builtin knows it.
#  2. x86-64, AVR, RX and ColdFire emit nothing (as GCC does on x86);
#     every other target calls libgcc's __clear_cache.
#  3. Every runtime defines __clear_cache (lib/rt/clear_cache.c): each
#     architecture's own sequence, checked here by its instructions.
set -u
echo "TEST-MARKER builtin-clear-cache"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/builtin-clear-cache
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

printf 'void f(int a) { __builtin___clear_cache(a, a); }\n' > "$out/bad.c"
if "$EMBCC" --target=riscv32-unknown-elf -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
    fail "an int was accepted"
fi
grep -q "takes two pointers, as in GCC, not int" "$out/bad.err" || {
    cat "$out/bad.err"; fail "an int was refused, but not by name"; }
printf '#if __has_builtin(__builtin___clear_cache)\nint ok;\n#else\n#error no\n#endif\n' > "$out/has.c"
"$EMBCC" --target=riscv32-unknown-elf -c "$out/has.c" -o /dev/null || fail "__has_builtin does not know it"
echo "builtin-clear-cache: a non-pointer is refused by name; __has_builtin knows it"

printf 'char buf[64];\nvoid f(void) { __builtin___clear_cache(buf, buf + 64); }\n' > "$out/c.c"
for row in "x86_64-elf 0" "avr 0" "rx-elf 0" "m68k-elf 0" "riscv32-unknown-elf 1" \
           "aarch64-elf 1" "thumbv7m-none-eabi 1" "mips-elf 1" "powerpc-eabi 1" \
           "aarch64-apple-darwin 1"; do
    t=${row% *}; want=${row#* }
    "$EMBCC" --target=$t -O2 -c "$out/c.c" -o "$out/c-$t.o" || fail "$t: does not compile"
    n=$(llvm-nm "$out/c-$t.o" | grep -c ' U _*__clear_cache$')
    [ "$n" = "$want" ] || fail "$t: $n calls to __clear_cache, not $want"
done
echo "builtin-clear-cache: nothing on x86-64, AVR, RX and ColdFire; a call to __clear_cache elsewhere"

# each runtime's __clear_cache, by the instructions it must hold
has() {             # has DIR PATTERN...
    a=build/libc/$1/librt.a
    [ -f "$a" ] || fail "$a: not built (make rt-embedded)"
    llvm-nm "$a" 2>/dev/null | grep -qE ' T _?__clear_cache$' || fail "$a: no __clear_cache"
    d=$1; shift
    for p in "$@"; do
        llvm-objdump -d "build/libc/$d/librt.a" 2>/dev/null | grep -q "$p" ||
            fail "$d: __clear_cache has no $p"
    done
}
has aarch64 "dc	cvau" "ic	ivau" "isb"
has armv7a-none-eabi "mcr	p15" "isb"
has thumbv7m-none-eabi "dsb" "isb"
has riscv32-unknown-elf "fence.i"
has mips-none-elf "synci" "jr.hb"
has powerpc-none-eabi "icbi" "isync"
has sparc-none-elf "flush"
has loongarch64-unknown-elf "ibar"
echo "builtin-clear-cache: every runtime defines __clear_cache, with its architecture's sequence"
