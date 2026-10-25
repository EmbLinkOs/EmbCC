#!/bin/sh
# clang's __builtin_debugtrap(): the target's own breakpoint instruction,
# inline at the call (sema.c debugtrap_insn), on every target and OS --
# a breakpoint the program carries on from, which no board here can run
# without a debugger attached, so this is about the bytes.
#
#  1. __has_builtin knows it; an argument is refused by name.
#  2. Each target's function carries its breakpoint: graded by
#     llvm-objdump's mnemonic where LLVM disassembles the target, and by
#     the instruction's bytes where it does not (TriCore, Xtensa, RX,
#     ColdFire), each worked from the architecture manual's format.
#  3. It is inline: no call, nothing for a runtime to provide -- so
#     Darwin and Windows, linked against the system's libraries, have it.
#  4. On a Darwin arm64 host, live: run alone it stops the program with
#     SIGTRAP; under lldb it stops there, and a `continue` carries on to
#     the end -- what clang's does.
set -u
echo "TEST-MARKER builtin-debugtrap"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/builtin-debugtrap
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

printf '#if __has_builtin(__builtin_debugtrap)\nint ok;\n#else\n#error no\n#endif\n' > "$out/has.c"
"$EMBCC" --target=riscv32-unknown-elf -c "$out/has.c" -o /dev/null ||
    fail "__has_builtin does not know it"
printf 'void f(void) { __builtin_debugtrap(1); }\n' > "$out/bad.c"
if "$EMBCC" --target=riscv32-unknown-elf -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
    fail "__builtin_debugtrap(1) was accepted"
fi
grep -q "takes no arguments" "$out/bad.err" || {
    cat "$out/bad.err"; fail "__builtin_debugtrap(1) was refused, but not by name"; }
echo "builtin-debugtrap: __has_builtin knows it; an argument is refused by name"

# a store on either side, so the breakpoint sits inside a body
printf 'int g;\nint f(int x) { g = x; __builtin_debugtrap(); return g + 1; }\n' > "$out/d.c"
n=0
for row in "x86_64-elf int3" "x86_64-windows-gnu int3" \
           "aarch64-elf brk	#0xf000" "aarch64-apple-darwin brk	#0xf000" \
           "thumbv7m-none-eabi bkpt	#0x0" "armv7a-none-eabi bkpt	#0x0" \
           "riscv32-unknown-elf ebreak" "riscv64-unknown-elf ebreak" \
           "avr break" "mips-elf break" "mips64-elf break" \
           "loongarch64-elf break	0" "powerpc-eabi trap" "sparc-elf ta	1"; do
    t=${row%% *}; insn=${row#* }
    "$EMBCC" --target=$t -O2 --no-warn=windows-abi -c "$out/d.c" -o "$out/d-$t.o" ||
        fail "$t: does not compile"
    llvm-objdump -d "$out/d-$t.o" > "$out/d-$t.dis" || fail "$t: llvm-objdump"
    grep -q "	$insn" "$out/d-$t.dis" || {
        cat "$out/d-$t.dis"; fail "$t: no '$insn' in f"; }
    if llvm-nm "$out/d-$t.o" | grep -q ' U '; then
        llvm-nm "$out/d-$t.o"; fail "$t: f calls something"
    fi
    n=$((n + 1))
done

# Targets LLVM has no disassembler for: the function's -S bytes, which
# hold the instruction where a body without it does not.
#   TriCore DEBUG, SYS format: op1 0x0d, op2 0x04 at bits 22-27 -> 0x0100000d
#   Xtensa BREAK 1, 15, RRR: op0 0, t 15, s 1, r 4 -> f0 41 00
#   RX BRK -> 00, then the RTS (02) that is all of an empty f
#   ColdFire HALT -> 4a c8
printf 'void f(void) { __builtin_debugtrap(); }\n' > "$out/one.c"
for row in "tricore-none-elf 0x0d,0x00,0x00,0x01" "xtensa-esp32-elf 0xf0,0x41,0x00" \
           "rx-elf ^0x00,0x02," "m68k-elf 0x4a,0xc8"; do
    t=${row%% *}; bytes=${row#* }
    "$EMBCC" --target=$t -O2 -S "$out/one.c" -o "$out/one-$t.s" || fail "$t: does not compile"
    sed -n '/^_\{0,1\}f:/,/\.size/p' "$out/one-$t.s" | grep '\.byte' | tr -d ' \t' |
        sed 's/\.byte//' | tr '\n' ',' > "$out/one-$t.bytes"
    grep -q "$bytes" "$out/one-$t.bytes" || {
        cat "$out/one-$t.s"; fail "$t: f does not hold $bytes"; }
    n=$((n + 1))
done
echo "builtin-debugtrap: the target's breakpoint, inline, on $n targets (Darwin and Windows included)"

[ "$(uname -s)-$(uname -m)" = Darwin-arm64 ] && command -v lldb > /dev/null 2>&1 || {
    echo "builtin-debugtrap: the live check needs a Darwin arm64 host with lldb (skipped)"
    exit 0; }
# bounded SECS OUT CMD...: CMD, killed after SECS (a debugger that hangs
# must not hang the suite)
bounded() {
    secs=$1; o=$2; shift 2
    "$@" < /dev/null > "$o" 2>&1 &
    bp=$!
    (
        trap 'kill "$s" 2>/dev/null; exit 0' TERM
        sleep "$secs" & s=$!
        wait "$s"
        kill -9 "$bp" 2>/dev/null
    ) </dev/null >/dev/null 2>&1 &
    wd=$!
    wait "$bp"; st=$?
    kill "$wd" 2>/dev/null
    wait "$wd" 2>/dev/null
    return $st
}
printf '#include <stdio.h>\nint main(void) { puts("before"); __builtin_debugtrap();\n    puts("after"); return 7; }\n' > "$out/live.c"
"$EMBCC" --target=aarch64-apple-darwin "$out/live.c" -o "$out/live" || fail "darwin: does not link"
# (under a shell of its own, whose report of the signal lands in the file)
bounded 30 "$out/alone.out" sh -c '"$0"; exit $?' "$out/live"; st=$?
[ "$st" = 133 ] && ! grep -q after "$out/alone.out" || {
    cat "$out/alone.out"; fail "darwin: alone it exited $st, not by SIGTRAP (133)"; }
# (-k: lldb --batch runs these only once the process has stopped on a signal)
bounded 60 "$out/lldb.out" lldb --batch -o "process launch" \
    -k "process continue" -k quit -- "$out/live"
grep -q "stop reason = EXC_BREAKPOINT" "$out/lldb.out" &&
grep -q "^after" "$out/lldb.out" &&
grep -q "exited with status = 7" "$out/lldb.out" || {
    cat "$out/lldb.out"; fail "darwin: under lldb it did not stop and carry on"; }
echo "builtin-debugtrap: on this Darwin host it stops alone by SIGTRAP; under lldb it stops and carries on"
