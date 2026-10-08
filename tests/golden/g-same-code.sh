#!/bin/sh
# -g changes no code. An object built with -g, once its debug sections
# are stripped, must be byte-identical to the same object built without
# -g, stripped the same way: the code, the data, the symbols and the
# relocations all agree. That is what lets a firmware be debugged with
# the ELF of the very image that was flashed. It used not to hold: -g
# kept every variable in its frame slot, so .text differed in nine
# programs out of ten at -O1 and above, on every target.
#
# Every target, every level (-Og included), a sample of tests/exec chosen
# for what -g once changed: parameters, locals, floats, aggregates,
# loops, calls and varargs. src/debug/dwarf.c describes whatever the
# code does instead (an empty location for a variable the optimizer took
# out of its slot).
set -u
echo "TEST-MARKER g-same-code"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/g-same-code
rm -rf "${out:?}"; mkdir -p "$out"
# llvm-objcopy reads every target's ELF; binutils' x86-64 build does not
LOBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
command -v "$LOBJCOPY" >/dev/null 2>&1 || {
    echo "SKIP: llvm-objcopy not found (set EMBCC_LLVM_OBJCOPY)"; exit 0; }

TARGETS="x86_64-elf aarch64-elf thumbv7em-none-eabi thumbv6m-none-eabi
         thumbv7em-none-eabihf armv7a-none-eabi riscv32-unknown-elf
         riscv64-unknown-elf avr mipsel-none-elf mips64-none-elf
         loongarch64-none-elf tricore-none-elf xtensa-none-elf
         powerpc-none-eabi sparc-none-elf m68k-none-elf rx-none-elf"
PROGS="hello structs floats loops va-arg recursion switch-thread
       pointers bitfields args-locals many-params tailcall vla"
fail=0; n=0; nprog=0
for p in $PROGS; do
    [ -f "tests/exec/$p.c" ] && nprog=$((nprog + 1))
done
[ "$nprog" -ge 12 ] || { echo "FAIL: only $nprog of the sample programs exist"; exit 1; }
for t in $TARGETS; do
    for o in -O0 -O1 -O2 -Os -Og; do
        for p in $PROGS; do
            src=tests/exec/$p.c
            [ -f "$src" ] || continue
            "$EMBCC" --target="$t" $o -c "$src" -o "$out/n.o" 2>/dev/null || continue
            if ! "$EMBCC" --target="$t" $o -g -c "$src" -o "$out/g.o" 2>"$out/g.err"; then
                echo "FAIL: $t $o $p compiles without -g but not with it:"
                head -3 "$out/g.err"; fail=1; continue
            fi
            "$LOBJCOPY" --strip-debug "$out/n.o" "$out/n.s" &&
                "$LOBJCOPY" --strip-debug "$out/g.o" "$out/g.s" ||
                { echo "FAIL: objcopy on $t $o $p"; fail=1; continue; }
            n=$((n + 1))
            if ! cmp -s "$out/n.s" "$out/g.s"; then
                echo "FAIL: $t $o $p: -g changed the object"
                fail=1
            fi
        done
    done
done
[ "$n" -ge 500 ] || { echo "FAIL: only $n objects compared"; fail=1; }
[ $fail = 0 ] && echo "-g changes no code: $n objects identical with and without it, 18 targets, -O0 to -Og"
exit $fail
