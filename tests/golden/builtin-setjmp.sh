#!/bin/sh
# GCC's __builtin_setjmp and __builtin_longjmp: what is refused, and what
# they are lowered onto. tests/exec/builtin-setjmp.c runs them on every
# board (exec-boards.sh and each target's exec golden, avr-setjmp.sh on
# the ATmega328P).
#
#  1. Refused by name: __builtin_longjmp with anything but the constant 1
#     (GCC's rule), a buffer that is not a pointer, and both on Windows,
#     whose _setjmp takes a frame for its unwinder.
#  2. __has_builtin knows both.
#  3. A __builtin_setjmp calls lib/rt's __embcc_setjmp on a freestanding
#     or EmbCC-libc target and the system's _setjmp on Darwin; every
#     embedded triple's librt.a defines __embcc_setjmp and __embcc_longjmp
#     (lib/rt/builtin-setjmp.c, lib/libc's setjmp under those names).
set -u
echo "TEST-MARKER builtin-setjmp"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/builtin-setjmp
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

refc() {            # refc TRIPLE WHAT PATTERN SOURCE
    printf '%s\n' "$4" > "$out/bad.c"
    if "$EMBCC" --target=$1 -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        fail "$2 was accepted on $1"
    fi
    grep -q -- "$3" "$out/bad.err" || {
        cat "$out/bad.err"; fail "$2 was refused on $1, but not by name"; }
}
refc riscv32-unknown-elf "__builtin_longjmp(b, 2)" "second argument must be the constant 1" \
    'void *b[5]; void f(void) { __builtin_longjmp(b, 2); }'
refc riscv32-unknown-elf "__builtin_setjmp(an int)" "buffer is a pointer" \
    'int x; int f(void) { return __builtin_setjmp(x); }'
refc x86_64-windows-gnu "__builtin_setjmp on Windows" "MinGW's setjmp takes a frame" \
    'void *b[5]; int f(void) { return __builtin_setjmp(b); }'
echo "builtin-setjmp: a longjmp of other than 1, a non-pointer buffer and Windows are refused by name"

printf '#if __has_builtin(__builtin_setjmp) && __has_builtin(__builtin_longjmp)\nint ok;\n#else\n#error no\n#endif\n' > "$out/has.c"
"$EMBCC" --target=riscv32-unknown-elf -c "$out/has.c" -o /dev/null ||
    fail "__has_builtin does not know them"

printf 'void *b[5]; int f(void) { return __builtin_setjmp(b); }\nvoid g(void) { __builtin_longjmp(b, 1); }\n' > "$out/c.c"
for t in riscv32-unknown-elf aarch64-apple-darwin; do
    "$EMBCC" --target=$t -c "$out/c.c" -o "$out/c-$t.o" || fail "$t: does not compile"
done
llvm-nm "$out/c-riscv32-unknown-elf.o" | grep -q ' U __embcc_setjmp$' &&
llvm-nm "$out/c-riscv32-unknown-elf.o" | grep -q ' U __embcc_longjmp$' ||
    fail "riscv32: not a call to lib/rt's __embcc_setjmp and __embcc_longjmp"
llvm-nm "$out/c-aarch64-apple-darwin.o" | grep -q ' U __setjmp$' &&
llvm-nm "$out/c-aarch64-apple-darwin.o" | grep -q ' U __longjmp$' ||
    fail "darwin: not a call to the system's _setjmp and _longjmp"
n=0
for a in build/libc/*/librt.a; do
    case $a in */linux-*|*/darwin-*|*/x86_64/*|*/aarch64/*) continue ;; esac
    # (RX's names carry its underscore, as its C names do)
    llvm-nm "$a" 2>/dev/null | grep -qE ' T _?__embcc_setjmp$' &&
    llvm-nm "$a" 2>/dev/null | grep -qE ' T _?__embcc_longjmp$' ||
        fail "$a does not define __embcc_setjmp and __embcc_longjmp (make rt-embedded)"
    n=$((n + 1))
done
[ "$n" -gt 10 ] || fail "only $n embedded runtimes were found (make rt-embedded)"
echo "builtin-setjmp: lowered onto lib/rt's pair (the system's on Darwin); $n embedded runtimes define it"
