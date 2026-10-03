#!/bin/sh
# A builtin the machine has no instruction for is a call to the library
# function of the same name, as gcc makes it, and __has_builtin says so.
#
# __builtin_sqrt was refused by the Cortex-M, RISC-V and AVR backends
# ("a libm routine here, not an instruction") while __has_builtin
# answered 1 for it, and __builtin_sqrtl was typed double on every target,
# so on x86-64 and AArch64 a long double lost its low bits to a double
# square root. The instruction is used where there is one: sqrtss/sqrtsd,
# fsqrt, and vsqrt.f32 on a Cortex-M FPU (single precision only).
#
# Also checked here: a NASM .asm input for a target other than x86-64 ELF
# is refused, where it was assembled as x86-64 anyway.
set -u
echo "TEST-MARKER builtin-libcalls"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/builtin-libcalls
rm -rf "${out:?}"; mkdir -p "$out"
command -v llvm-readelf > /dev/null 2>&1 || { echo "SKIP: no llvm-readelf"; exit 0; }

cat > "$out/s.c" <<'SRC'
#if !__has_builtin(__builtin_sqrt) || !__has_builtin(__builtin_sqrtf) || \
    !__has_builtin(__builtin_sqrtl)
#error the square-root builtins are always available
#endif
double d(double x) { return __builtin_sqrt(x); }
float f(float x) { return __builtin_sqrtf(x); }
#ifndef NO_LDOUBLE
long double l(long double x) { return __builtin_sqrtl(x); }
#endif
SRC

# target | extra flags | the libm calls the object must make
check() {
    t=$1; fl=$2; want=$3
    # shellcheck disable=SC2086
    "$EMBCC" --target=$t $fl -O2 -c "$out/s.c" -o "$out/s.o" \
        > "$out/cc.log" 2>&1 || {
        echo "FAIL $t: did not compile"; cat "$out/cc.log"; exit 1; }
    got=$(llvm-readelf -r "$out/s.o" | grep -o 'sqrt[fl]*' | sort -u | tr '\n' ' ')
    [ "$got" = "$want" ] || {
        echo "FAIL $t $fl: calls '$got', expected '$want'"; exit 1; }
}
check x86_64-elf "" "sqrtl "
check aarch64-elf "" "sqrtl "
check thumbv7m-none-eabi "" "sqrt sqrtf sqrtl "
check thumbv7em-none-eabihf "" "sqrt sqrtl "
check riscv32-unknown-elf -DNO_LDOUBLE "sqrt sqrtf "
check riscv64-unknown-elf -DNO_LDOUBLE "sqrt sqrtf "
check avr "" "sqrt sqrtf sqrtl "
echo "builtin-libcalls: sqrt is the instruction where there is one, else the libm call"

# ...which, inside that libm function, is a call to ITSELF. lib/libc's sqrt
# was `return __builtin_sqrt(x);`, and on every embedded target sqrt,
# hypot and cabs never returned. The shape is refused by name, and libc's
# own sqrt.c has to compile everywhere.
printf 'double sqrt(double x) { return __builtin_sqrt(x); }\n' > "$out/self.c"
for t in thumbv7m-none-eabi thumbv7em-none-eabihf riscv32-unknown-elf \
         riscv64-unknown-elf avr; do
    if "$EMBCC" --target=$t -c "$out/self.c" -o "$out/self.o" \
         2> "$out/self.err"; then
        echo "FAIL $t: a sqrt that calls itself compiled"; exit 1
    fi
    grep -q 'calls sqrt itself' "$out/self.err" || {
        echo "FAIL $t: the refusal does not say why:"; cat "$out/self.err"
        exit 1; }
    "$EMBCC" --target=$t -Os -Ilib/libc/include \
        -c lib/libc/src/math/sqrt.c -o "$out/sqrt.o" || {
        echo "FAIL $t: lib/libc/src/math/sqrt.c does not compile"; exit 1; }
done
"$EMBCC" --target=x86_64-elf -c "$out/self.c" -o "$out/self.o" || {
    echo "FAIL: x86-64 has sqrtsd, and refused the builtin"; exit 1; }
echo "builtin-libcalls: a sqrt whose builtin would call itself is refused"

printf 'section .text\nglobal f\nf: ret\n' > "$out/a.asm"
for t in thumbv7m-none-eabi riscv32-unknown-elf aarch64-elf x86_64-apple-darwin; do
    if "$EMBCC" --target=$t -c "$out/a.asm" -o "$out/a.o" 2> "$out/a.err"; then
        echo "FAIL $t: a NASM .asm was assembled"; exit 1
    fi
    grep -q 'NASM-syntax x86-64' "$out/a.err" || {
        echo "FAIL $t: the refusal does not say why:"; cat "$out/a.err"; exit 1; }
done
"$EMBCC" --target=x86_64-elf -c "$out/a.asm" -o "$out/a.o" || {
    echo "FAIL: x86-64 ELF no longer assembles .asm"; exit 1; }
echo "builtin-libcalls: .asm is refused for every target but x86-64 ELF"
