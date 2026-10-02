#!/bin/sh
# The operators on an _Atomic object are atomic, on every target.
#
# C11 gives `x++`, `x op= v`, `x = v` and a read of an _Atomic object
# seq_cst atomic semantics. EmbCC used to treat _Atomic as volatile, so
# `x++` became a load, an add and a store -- an increment an interrupt or
# another core could lose -- with nothing said. Each function below must
# now contain the target's atomic read-modify-write (or, for *=, its
# compare-and-swap), and a seq_cst load and store their barriers on the
# weakly ordered targets. tests/exec/atomic-operators.c checks the values.
set -u
echo "TEST-MARKER atomic-operators"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/atomic-operators
rm -rf "$out"; mkdir -p "$out"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }

cat > "$out/a.c" <<'CEOF'
_Atomic int x;
int f_inc(void)   { return x++; }
int f_add(int v)  { return x += v; }
int f_or(int v)   { return x |= v; }
int f_mul(int v)  { return x *= v; }
void f_st(int v)  { x = v; }
int f_ld(void)    { return x; }
CEOF

fail=0
# target, objdump flags, the RMW pattern, the CAS pattern, and the
# barrier (empty: none expected around a plain load/store)
check() {
    T=$1 ODF=$2 RMW=$3 CAS=$4 BAR=$5
    for opt in -O0 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$out/a.c" -o "$out/a.o" || {
            echo "FAIL $T $opt: does not compile"; fail=1; continue; }
        # shellcheck disable=SC2086
        "$OD" -d $ODF "$out/a.o" > "$out/a.s"
        for f in f_inc f_add f_or f_mul; do
            pat=$RMW; [ $f = f_mul ] && pat=$CAS
            sed -n "/<$f>:/,/^\$/p" "$out/a.s" | grep -qE "$pat" || {
                echo "FAIL $T $opt $f: no atomic read-modify-write ($pat)"
                fail=1; }
        done
        if [ -n "$BAR" ]; then
            for f in f_st f_ld; do
                sed -n "/<$f>:/,/^\$/p" "$out/a.s" | grep -qE "$BAR" || {
                    echo "FAIL $T $opt $f: a seq_cst access with no barrier"
                    fail=1; }
            done
        fi
    done
}
check thumbv7m-none-eabi "--triple=thumbv7m" 'ldrex' 'ldrex' 'dmb'
check riscv32-unknown-elf "--mattr=+a,+m,+c" 'amo(add|or)\.w' 'lr\.w' 'fence'
check aarch64-elf "" '(ld[a]?xr|ldadd|ldset)' '(ld[a]?xr|cas)' 'dmb'
check x86_64-elf "" 'lock' 'lock' ''
# What a target cannot do in one access is refused, not done in pieces:
# an 8-byte atomic on a 32-bit core (the __atomic builtin's load was two
# plain loads), a floating _Atomic's read-modify-write.
refuse() {
    T=$1 SRC=$2 MSG=$3
    printf '%s\n' "$SRC" > "$out/r.c"
    if "$EMBCC" --target=$T -O2 -c "$out/r.c" -o "$out/r.o" 2> "$out/r.txt"; then
        echo "FAIL $T: compiled: $SRC"; fail=1
    elif ! grep -q "$MSG" "$out/r.txt"; then
        echo "FAIL $T: wrong diagnostic for: $SRC"; sed 's/^/     | /' "$out/r.txt"
        fail=1
    fi
}
refuse thumbv7m-none-eabi '_Atomic long long y; long long f(void) { return y; }' \
       "not one access"
refuse thumbv7m-none-eabi 'long long y; long long f(void) { return __atomic_load_n(&y, 5); }' \
       "not one access"
refuse x86_64-elf '_Atomic double d; void f(void) { d += 1.0; }' \
       "integers and pointers only"
[ "$fail" -eq 0 ] || exit 1
echo "x++, x += v and x |= v on an _Atomic int are one atomic
read-modify-write, x *= v a compare-and-swap, and a seq_cst load and
store carry their barriers, on Thumb, RISC-V, aarch64 and x86-64 at -O0,
-O2 and -Os; an access a target cannot make in one go is refused"
