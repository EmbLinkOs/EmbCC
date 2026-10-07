#!/bin/sh
# ARMv8-M Baseline: the Cortex-M23, thumbv8m.base-none-eabi.
#
# A LEVEL of the Thumb target -- level 6, ARMv6-M's instruction selection
# (src/arch/thumb/v6m.c), with a flag for what Baseline adds -- and that is
# what this test pins:
#
#   the triple        three spellings, one canonical name; -mcpu=cortex-m23
#                     selects it on any Thumb triple, as clang does, and
#                     -mcpu=cortex-m0 leaves it
#   the data model    ARMv6-M's (ILP32, AAPCS32)
#   the macros        clang's for -mcpu=cortex-m23 (tests/golden/predef.sh
#                     compares the whole table), less the five it overclaims
#   the object        Tag_CPU_arch 16 (v8-M Baseline), Tag_THUMB_ISA_use 3,
#                     no unaligned access
#   the code          a 32-bit divide and remainder are SDIV/UDIV, and a
#                     one-, two- or four-byte atomic an LDREX/STREX loop --
#                     no __aeabi_idiv and no __atomic_* call -- where the same
#                     source for ARMv6-M still calls them
#   the refusals      an eight-byte atomic (there is no LDREXD), an FPU
set -u
echo "TEST-MARKER thumbv8mbase-target"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/thumbv8mbase-target
rm -rf "$out"; mkdir -p "$out"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
T=thumbv8m.base-none-eabi
fail=0

# ---- the triple ---------------------------------------------------------
for t in thumbv8m.base-none-eabi thumbv8m.base armv8m.base-none-eabi; do
    got=$("$EMBCC" --target=$t -dumpmachine 2>/dev/null)
    [ "$got" = $T ] || { echo "--target=$t is '$got', not $T"; fail=1; }
done
got=$("$EMBCC" --target=thumbv6m-none-eabi -mcpu=cortex-m23 -dumpmachine)
[ "$got" = $T ] || { echo "-mcpu=cortex-m23 on thumbv6m gives '$got'"; fail=1; }
got=$("$EMBCC" --target=$T -mcpu=cortex-m0 -dumpmachine)
[ "$got" = thumbv6m-none-eabi ] || {
    echo "-mcpu=cortex-m0 on $T gives '$got', not thumbv6m-none-eabi"; fail=1; }
got=$("$EMBCC" --target=$T -mcpu=cortex-m33 -dumpmachine)
[ "$got" = thumbv8m.main-none-eabi ] || {
    echo "-mcpu=cortex-m33 on $T gives '$got'"; fail=1; }
[ "$fail" = 0 ] && echo "the triple: three spellings, and -mcpu= moves between the levels"

cat > "$out/dm.c" <<'CEOF'
_Static_assert(sizeof(int) == 4, "");
_Static_assert(sizeof(long) == 4, "");
_Static_assert(sizeof(void *) == 4, "");
_Static_assert(sizeof(long long) == 8, "");
_Static_assert(sizeof(long double) == 8, "");
_Static_assert(_Alignof(long long) == 8, "");
_Static_assert((char)-1 > 0, "char is unsigned on ARM");
int ok;
CEOF
"$EMBCC" --target=$T -fsyntax-only "$out/dm.c" || {
    echo "the data model is not ARMv6-M's"; fail=1; }

# ---- the macros ------------------------------------------------------------
m=$("$EMBCC" --target=$T --dump-predef)
for want in '__ARM_ARCH 8' '__ARM_ARCH_8M_BASE__ 1' '__ARM_ARCH_PROFILE '"'M'" \
            '__ARM_ARCH_ISA_THUMB 1' '__ARM_FEATURE_IDIV 1' \
            '__ARM_FEATURE_LDREX 0x7' '__ARM_FEATURE_CMSE 1' \
            '__GCC_ATOMIC_INT_LOCK_FREE 2' '__SOFTFP__ 1'; do
    printf '%s\n' "$m" | grep -qx "#define $want" || {
        echo "missing: #define $want"; fail=1; }
done
for bad in __ARM_ARCH_6M__ __ARM_ARCH_8M_MAIN__ __ARM_FEATURE_CLZ \
           __ARM_FEATURE_SAT __ARM_FEATURE_QBIT __ARM_FEATURE_DSP \
           __ARM_FEATURE_NUMERIC_MAXMIN __ARM_FEATURE_DIRECTED_ROUNDING \
           __thumb2__ __ARM_FP; do
    printf '%s\n' "$m" | grep -q "^#define $bad " && {
        echo "claims $bad, which a Cortex-M23 does not have"; fail=1; }
done
[ "$fail" = 0 ] && echo "the macros say ARMv8-M Baseline, with the divide and the exclusives, and nothing it lacks"

# ---- the object ------------------------------------------------------------
printf 'int f(int a, int b) { return a / b; }\n' > "$out/f.c"
"$EMBCC" --target=$T -O1 -c "$out/f.c" -o "$out/f.o" || exit 1
if command -v llvm-readobj >/dev/null 2>&1; then
    llvm-readobj -A "$out/f.o" > "$out/attrs.txt" 2>/dev/null
    grep -q "ARM v8-M Baseline" "$out/attrs.txt" || {
        echo "Tag_CPU_arch is not ARM v8-M Baseline:"
        grep -A2 CPU_arch "$out/attrs.txt" | head -4; fail=1; }
    grep -B3 "TagName: THUMB_ISA_use" "$out/attrs.txt" | grep -q "Value: 3" || {
        echo "Tag_THUMB_ISA_use is not 3"; fail=1; }
    grep -B3 "TagName: CPU_unaligned_access" "$out/attrs.txt" |
        grep -q "Value: 0" || {
        echo "the object claims unaligned access, which faults on an M23"
        fail=1; }
    [ "$fail" = 0 ] && echo "the object reports ARM v8-M Baseline"
fi

# ---- the code: the divides and the exclusives --------------------------------
cat > "$out/k.c" <<'CEOF'
int sdiv_(int a, int b) { return a / b; }
unsigned udiv_(unsigned a, unsigned b) { return a / b; }
int smod_(int a, int b) { return a % b; }
unsigned umod_(unsigned a, unsigned b) { return a % b; }
int fadd(int *p, int v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
short xchg(short *p, short v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
unsigned char fand(unsigned char *p, unsigned char v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
int cas(int *p, int *e, int d) { return __atomic_compare_exchange_n(p, e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
char vcas(char *p, char e, char d) { return __sync_val_compare_and_swap(p, e, d); }
CEOF
for O in -O0 -O2 -Os; do
    "$EMBCC" --target=$T $O -c "$out/k.c" -o "$out/k$O.o" 2> "$out/k.err" || {
        echo "$O: the kernels do not compile:"; head -3 "$out/k.err"; fail=1
        continue; }
    "$OD" -dr --triple=thumbv8m.base "$out/k$O.o" > "$out/k$O.dis"
    for ins in sdiv udiv ldrex strex ldrexh strexh ldrexb strexb clrex dmb; do
        grep -Eq "	$ins(	|\$)" "$out/k$O.dis" || {
            echo "$O: no $ins in the code -- the Baseline instruction is not used"
            fail=1; }
    done
    if grep -E "R_ARM_THM_CALL.*(__aeabi_.*div|__atomic|__sync)" "$out/k$O.dis"; then
        echo "$O: a divide or an atomic is still a call on ARMv8-M Baseline"
        fail=1
    fi
done
# ...where ARMv6-M, which has neither, still calls
"$EMBCC" --target=thumbv6m-none-eabi -O2 -c "$out/k.c" -o "$out/k6.o"
"$OD" -dr --triple=thumbv6m "$out/k6.o" > "$out/k6.dis"
grep -q "R_ARM_THM_CALL.*__aeabi_idiv" "$out/k6.dis" &&
grep -q "R_ARM_THM_CALL.*__atomic_fetch_add_4" "$out/k6.dis" || {
    echo "ARMv6-M no longer calls the helpers it needs"; fail=1; }
[ "$fail" = 0 ] && echo "a 32-bit divide is sdiv/udiv and an atomic an ldrex/strex loop, at three levels; ARMv6-M still calls"

# ---- the refusals -----------------------------------------------------------------
printf 'long long f(long long *p) { return __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST); }\n' > "$out/a8.c"
if "$EMBCC" --target=$T -O1 -c "$out/a8.c" -o "$out/a8.o" 2> "$out/a8.err"; then
    echo "an eight-byte atomic compiled, and Baseline has no LDREXD"; fail=1
else
    grep -q "ARMv8-M Baseline backend cannot lower an atomic wider than four bytes" \
        "$out/a8.err" || { echo "the eight-byte atomic is refused, not by name:"
                           head -2 "$out/a8.err"; fail=1; }
fi
if "$EMBCC" --target=$T -mfpu=fpv5-sp-d16 -mfloat-abi=hard -c "$out/f.c" \
       -o "$out/fp.o" 2> "$out/fp.err"; then
    echo "-mfpu= was accepted for a Cortex-M23, which has no FPU"; fail=1
else
    grep -q "ARMv8-M Baseline core (Cortex-M23) has no FPU" "$out/fp.err" || {
        echo "-mfpu= refused, not by name:"; head -2 "$out/fp.err"; fail=1; }
fi
[ "$fail" = 0 ] && echo "an eight-byte atomic and an FPU are refused by name"
exit $fail
