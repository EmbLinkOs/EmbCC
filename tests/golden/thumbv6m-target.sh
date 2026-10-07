#!/bin/sh
# ARMv6-M: Cortex-M0, M0+ and M1, as a LEVEL on the Thumb target (as
# ARMv8-M Mainline is: thumbv8m-target.sh), selected by the triple or by
# -mcpu=cortex-m0/m0plus/m1 on any ARM triple.
#
# The data model and AAPCS32 are ARMv7-M's; what differs is the instruction
# set (Thumb-1, src/arch/thumb/v6m.c), what the preprocessor says and what
# the object says. Both of those are checked against clang for the triple:
#
#   __ARM_ARCH 6, __ARM_ARCH_6M__, __ARM_ARCH_ISA_THUMB 1, no IDIV, LDREX,
#   CLZ or unaligned access, atomics not lock-free (they are calls)
#   Tag_CPU_arch 12 (v6S-M), Tag_THUMB_ISA_use 1 (Thumb-1),
#   Tag_CPU_unaligned_access 0
#
# What the code COMPUTES is thumb-v6m-exec.sh.
set -u
echo "TEST-MARKER thumbv6m-target"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/thumbv6m-target
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}

# ---- the names ----------------------------------------------------------
for t in thumbv6m-none-eabi thumbv6m armv6m-none-eabi; do
    got=$("$EMBCC" --target=$t -dumpmachine 2>/dev/null) || {
        echo "--target=$t is not recognised"; exit 1; }
    [ "$got" = "thumbv6m-none-eabi" ] || {
        echo "--target=$t canonicalises to '$got'"; exit 1; }
done
for cpu in cortex-m0 cortex-m0plus cortex-m1; do
    for t in arm-none-eabi thumbv7m-none-eabi; do
        got=$("$EMBCC" --target=$t -mcpu=$cpu -dumpmachine)
        [ "$got" = "thumbv6m-none-eabi" ] || {
            echo "$t -mcpu=$cpu selects '$got'"; exit 1; }
    done
done
got=$("$EMBCC" --target=thumbv6m-none-eabi -mcpu=cortex-m3 -dumpmachine)
[ "$got" = "thumbv7m-none-eabi" ] || {
    echo "-mcpu=cortex-m3 on the ARMv6-M triple selects '$got'"; exit 1; }
# The Cortex-M23 is ARMv8-M Baseline, a level of its own now
# (tests/golden/thumbv8mbase-target.sh), not ARMv6-M.
got=$("$EMBCC" --target=thumbv6m-none-eabi -mcpu=cortex-m23 -dumpmachine)
[ "$got" = "thumbv8m.base-none-eabi" ] || {
    echo "-mcpu=cortex-m23 on the ARMv6-M triple selects '$got'"; exit 1; }
echo "the triples and -mcpu=cortex-m0/m0plus/m1 select ARMv6-M; the M23 selects ARMv8-M Baseline"

# ---- the data model is ARMv7-M's ------------------------------------------
cat > "$out/dm.c" <<'CEOF'
_Static_assert(sizeof(int) == 4, "");
_Static_assert(sizeof(long) == 4, "");
_Static_assert(sizeof(void *) == 4, "");
_Static_assert(sizeof(long long) == 8, "");
_Static_assert(_Alignof(long long) == 8, "");
_Static_assert(sizeof(double) == 8, "");
_Static_assert((char)-1 > 0, "char is unsigned on ARM");
int ok;
CEOF
"$EMBCC" --target=thumbv6m-none-eabi -fsyntax-only "$out/dm.c" || {
    echo "the ARMv6-M data model is not ARMv7-M's"; exit 1; }

# ---- the preprocessor, against clang -------------------------------------
v6=$("$EMBCC" --target=thumbv6m-none-eabi --dump-predef)
for m in '__ARM_ARCH 6' '__ARM_ARCH_6M__ 1' '__ARM_ARCH_ISA_THUMB 1' \
         '__ARM_ARCH_PROFILE '"'M'" '__GCC_ATOMIC_INT_LOCK_FREE 1'; do
    printf '%s\n' "$v6" | grep -q "^#define $m\$" || {
        echo "ARMv6-M does not define $m"; exit 1; }
done
for m in __ARM_ARCH_7M__ __ARM_FEATURE_IDIV __ARM_FEATURE_LDREX \
         __ARM_FEATURE_CLZ __ARM_FEATURE_UNALIGNED __thumb2__ \
         __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4 __ARM_FP; do
    printf '%s\n' "$v6" | grep -q "^#define $m " && {
        echo "ARMv6-M claims $m, which it does not have"; exit 1; }
done
if command -v clang >/dev/null 2>&1; then
    ref=$(sh tools/gen-predef.sh --reference thumbv6m)
    [ "$v6" = "$ref" ] || {
        echo "the ARMv6-M table disagrees with clang's:"
        printf '%s\n' "$v6" > "$out/v6.txt"
        printf '%s\n' "$ref" | diff -u - "$out/v6.txt" | head -20; exit 1; }
    echo "the ARMv6-M macros are clang's for thumbv6m-none-eabi ($(printf '%s\n' "$v6" | wc -l | tr -d ' '))"
fi

# ---- the object, against clang --------------------------------------------
printf 'int f(int a, int b) { return a * b + 1; }\n' > "$out/f.c"
"$EMBCC" --target=thumbv6m-none-eabi -O1 -c "$out/f.c" -o "$out/f.o" || {
    echo "compiling for ARMv6-M failed"; exit 1; }
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
if command -v "$RE" >/dev/null 2>&1 && command -v clang >/dev/null 2>&1; then
    clang --target=thumbv6m-none-eabi -c "$out/f.c" -o "$out/ref.o"
    # The tags that say what the code may use, read back by an independent
    # reader from each object and compared value for value.
    tags() {
        "$RE" --arch-specific "$1" | awk '
            /Value:/       { v = $2 }
            /TagName:/     { t = $2 }
            /}/            { if (t != "") print t, v; t = "" }' |
        grep -E '^(CPU_arch|CPU_arch_profile|ARM_ISA_use|THUMB_ISA_use|CPU_unaligned_access|ABI_PCS_wchar_t|ABI_enum_size|ABI_align_needed|ABI_align_preserved|ABI_PCS_R9_use|ABI_FP_number_model) '
    }
    tags "$out/f.o" | sort > "$out/ours.txt"
    tags "$out/ref.o" | sort > "$out/clang.txt"
    # clang leaves Tag_CPU_unaligned_access out when it is 0 on some
    # versions; compare what both state, and require ours to state it.
    grep -q '^CPU_unaligned_access 0' "$out/ours.txt" || {
        echo "Tag_CPU_unaligned_access is not 0: ARMv6-M faults on one"
        cat "$out/ours.txt"; exit 1; }
    grep -v '^CPU_unaligned_access' "$out/ours.txt" > "$out/o2.txt"
    grep -v '^CPU_unaligned_access' "$out/clang.txt" > "$out/c2.txt"
    diff -u "$out/c2.txt" "$out/o2.txt" > "$out/tags.diff" || {
        echo "the object's tags disagree with clang's:"; cat "$out/tags.diff"
        exit 1; }
    echo "the object's tags are clang's: $(tr '\n' ',' < "$out/o2.txt")"
fi

# ---- the mapping symbols ---------------------------------------------------
# A literal pool can follow a switch table with no instruction between them
# (v6m.c places a pool after an unconditional transfer, and a table's `add
# pc` is one). Both are data: one `$d`, and no `$t` where the table ends,
# or a disassembler is left to choose between two mapping symbols at one
# address and decodes the pool as instructions. The program puts the pool
# right after a four-entry table at -O1 and -O2.
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
if command -v "$OD" >/dev/null 2>&1; then
    cat > "$out/tp.c" <<'CEOF'
volatile int v;
int f(int x)
{
    int k = v * 0x12345;          /* a literal: the pool's first word */
#define S v = v * 3 + k;
#define S8 S S S S S S S S
    S8 S8 S8 S8 S8 S8 S8 S8 S8
    switch (x) {
    case 0: return k + 1;
    case 1: return v + 7;
    case 2: return k ^ 3;
    case 3: return v - 9;
    }
    return 0;
}
CEOF
    for O in -O1 -O2 -Os; do
        "$EMBCC" --target=thumbv6m-none-eabi $O -c "$out/tp.c" \
            -o "$out/tp.o" || { echo "tp.c $O does not compile"; exit 1; }
        dup=$("$OD" -t "$out/tp.o" |
              awk '$NF ~ /^\$[td]$/ { k = $1; if (n[k]++) print k }')
        [ -z "$dup" ] || {
            echo "$O: two mapping symbols at $dup:"
            "$OD" -t "$out/tp.o" | grep -E '\$[td]$'; exit 1; }
        "$OD" -d "$out/tp.o" | grep -q '\.word	0x00012345' || {
            echo "$O: the literal pool is decoded as instructions:"
            "$OD" -d "$out/tp.o" | tail -30; exit 1; }
    done
    echo "a literal pool right after a switch table is one run of data"
fi
