#!/bin/sh
# The two RISC-V targets' DATA MODELS (D-016), and the refusal behind them.
#
# Neither width has a code generator yet, so what can be tested is exactly
# what exists: that the triples are accepted, that the front end believes
# the right things about each machine, and that asking for an object says
# so rather than producing one. The third is the point -- a wrong answer
# there would be an object full of x86-64 instructions under an EM_RISCV
# header.
#
# RV32 and RV64 are checked SEPARATELY throughout, and each assertion that
# distinguishes them is also run against the other width. One target
# standing in for two is how a second data model goes unnoticed.
set -u
echo "TEST-MARKER riscv-target"
. "$(dirname "$0")/../lib.sh"

R32=riscv32-unknown-elf
R64=riscv64-unknown-elf
tmp=${TMPDIR:-/tmp}/riscv-target.$$
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT

# 1. Every spelling of each target resolves, and the canonical name comes
#    back. `rv32`/`rv64` are here because that is what the architecture
#    manual calls them and what a person types.
for alias in riscv32-unknown-elf riscv32 riscv32-elf rv32; do
    got=$("$EMBCC" --target="$alias" -dumpmachine) || {
        echo "--target=$alias was not accepted"; exit 1; }
    [ "$got" = "$R32" ] || {
        echo "--target=$alias -dumpmachine said '$got', not '$R32'"; exit 1; }
done
for alias in riscv64-unknown-elf riscv64 riscv64-elf rv64; do
    got=$("$EMBCC" --target="$alias" -dumpmachine) || {
        echo "--target=$alias was not accepted"; exit 1; }
    [ "$got" = "$R64" ] || {
        echo "--target=$alias -dumpmachine said '$got', not '$R64'"; exit 1; }
done
echo "four spellings each resolve to $R32 and $R64"

# The other targets still answer for themselves. -dumpmachine reads the
# target table now instead of a ternary chain of its own, and a chain is
# exactly what printed x86_64-elf for every target added after it.
for pair in "x86_64-elf x86_64-elf" "aarch64-elf aarch64-elf" \
            "thumbv7m-none-eabi thumbv7m-none-eabi"; do
    set -- $pair
    got=$("$EMBCC" --target="$1" -dumpmachine)
    [ "$got" = "$2" ] || {
        echo "--target=$1 -dumpmachine regressed to '$got'"; exit 1; }
done
echo "and the three older targets are unchanged"

# 2. The data models, asked of the compiler rather than asserted about it.
#    _Static_assert over TYPE NAMES is what this front end folds in C; the
#    literal-typing rules go with the backends that can run them.
cat > "$tmp/ilp32.c" <<'EOF'
_Static_assert(sizeof(void *) == 4, "RV32 is ILP32: pointers are four bytes");
_Static_assert(sizeof(long) == 4, "ILP32: long is four bytes");
_Static_assert(sizeof(long long) == 8, "long long stays eight");
_Static_assert(sizeof(int) == 4 && sizeof(short) == 2, "int and short");
_Static_assert(sizeof(double) == 8, "double");
_Static_assert(sizeof(long double) == 16, "the psABI: long double is binary128");
EOF
"$EMBCC" --target="$R32" -fsyntax-only "$tmp/ilp32.c" || {
    echo "the RV32 data model is not what the psABI says"; exit 1; }
echo "RV32 holds ILP32 (ptr 4, long 4, long long 8, long double 16)"

cat > "$tmp/lp64.c" <<'EOF'
_Static_assert(sizeof(void *) == 8, "RV64 is LP64: pointers are eight bytes");
_Static_assert(sizeof(long) == 8, "LP64: long is eight bytes");
_Static_assert(sizeof(long long) == 8, "and long long is the same width");
_Static_assert(sizeof(int) == 4 && sizeof(short) == 2, "int and short");
_Static_assert(sizeof(long double) == 16, "the psABI: long double is binary128");
EOF
"$EMBCC" --target="$R64" -fsyntax-only "$tmp/lp64.c" || {
    echo "the RV64 data model is not what the psABI says"; exit 1; }
echo "RV64 holds LP64  (ptr 8, long 8, long long 8, long double 16)"

# Each width's file must FAIL on the other -- otherwise both are passing
# for some reason other than the target being read, which is what happens
# when a new arch enum lands without its own row in the data-model table.
if "$EMBCC" --target="$R64" -fsyntax-only "$tmp/ilp32.c" 2>/dev/null; then
    echo "the ILP32 assertions also hold on RV64 -- the widths are the same"
    exit 1
fi
if "$EMBCC" --target="$R32" -fsyntax-only "$tmp/lp64.c" 2>/dev/null; then
    echo "the LP64 assertions also hold on RV32 -- the widths are the same"
    exit 1
fi
echo "and neither model holds on the other width"

# 3. long double is IEEE binary128 on both, and that is a statement about
#    the FORMAT, not just the size: x86-64's long double is also sixteen
#    bytes and is not the same type. The C front end does not fold
#    floating-point into an integer constant expression, but the C++ one
#    evaluates it through the same ldf_target_fmt() the code generator
#    will use -- so this is the real thing being asked, not a proxy.
#
#    1e-20 is below x87's 2^-64 epsilon and above binary128's 2^-113, so
#    adding it to 1 is lost on x86-64 and kept here.
cat > "$tmp/fmt.cpp" <<'EOF'
static_assert(1.0L + 1e-20L != 1.0L, "binary128 keeps more than 64 mantissa bits");
EOF
for t in "$R32" "$R64"; do
    "$EMBCC" --target="$t" -fsyntax-only "$tmp/fmt.cpp" || {
        echo "$t folds long double as something narrower than binary128"
        exit 1; }
done
if "$EMBCC" --target=x86_64-elf -fsyntax-only "$tmp/fmt.cpp" 2>/dev/null; then
    echo "x86-64 kept the bit too -- x87 is not being used there"; exit 1
fi
if "$EMBCC" --target=thumbv7m-none-eabi -fsyntax-only "$tmp/fmt.cpp" 2>/dev/null; then
    echo "ARMv7-M kept the bit too -- its long double is supposed to be a double"
    exit 1
fi
echo "long double folds as binary128 on both widths, x87 on x86-64, double on ARMv7-M"

# 4. char is unsigned and wchar_t is SIGNED -- the combination is what
#    makes these two separate columns in the target table. aarch64 and
#    ARMv7-M have both unsigned; RISC-V does not.
for t in "$R32" "$R64"; do
    "$EMBCC" --target="$t" --dump-predef | grep -q '^#define __CHAR_UNSIGNED__ 1' || {
        echo "$t: __CHAR_UNSIGNED__ is missing"; exit 1; }
    "$EMBCC" --target="$t" --dump-predef | grep -q '^#define __WCHAR_UNSIGNED__' && {
        echo "$t: wchar_t is claimed unsigned; the psABI says it is signed"
        exit 1; }
    "$EMBCC" --target="$t" --dump-predef | grep -q '^#define __WCHAR_TYPE__ int$' || {
        echo "$t: __WCHAR_TYPE__ is not plain int"; exit 1; }
done
echo "char is unsigned and wchar_t is signed on both, as the psABI says"

# The predefined macros must agree with the front end's own sizes. They
# come from different places -- a generated table and the data-model row
# -- and nothing but a test makes them say the same thing.
"$EMBCC" --target="$R32" --dump-predef | grep -q '^#define __SIZEOF_POINTER__ 4' || {
    echo "RV32: __SIZEOF_POINTER__ disagrees with sizeof(void *)"; exit 1; }
"$EMBCC" --target="$R64" --dump-predef | grep -q '^#define __SIZEOF_POINTER__ 8' || {
    echo "RV64: __SIZEOF_POINTER__ disagrees with sizeof(void *)"; exit 1; }
"$EMBCC" --target="$R32" --dump-predef | grep -q '^#define __riscv_xlen 32' || {
    echo "RV32: __riscv_xlen is not 32"; exit 1; }
"$EMBCC" --target="$R64" --dump-predef | grep -q '^#define __riscv_xlen 64' || {
    echo "RV64: __riscv_xlen is not 64"; exit 1; }
echo "the predefined macros agree with the front end's own sizes"

# No hardware float is claimed: EmbCC has none for this target, and
# __riscv_flen is what a header reads to decide there is (THE RULE).
for t in "$R32" "$R64"; do
    "$EMBCC" --target="$t" --dump-predef | grep -q '^#define __riscv_float_abi_soft 1' || {
        echo "$t: the soft-float ABI is not claimed"; exit 1; }
    "$EMBCC" --target="$t" --dump-predef | grep -qE '^#define (__riscv_flen|__riscv_f |__riscv_d |__riscv_v )' && {
        echo "$t: a hardware float or vector extension is claimed and is not there"
        exit 1; }
done
echo "no hardware float and no vector extension is claimed"

# 5. __int128 exists at RV64 and not at RV32, and the 32-bit refusal is by
#    NAME rather than a lowering into something no backend has.
printf '__int128 x;\n' > "$tmp/i128.c"
"$EMBCC" --target="$R64" -fsyntax-only "$tmp/i128.c" || {
    echo "__int128 was refused on RV64, which has 64-bit registers"; exit 1; }
if "$EMBCC" --target="$R32" -fsyntax-only "$tmp/i128.c" 2>"$tmp/i128.err"; then
    echo "__int128 was accepted on RV32"; exit 1
fi
grep -q "__int128 does not exist on this target" "$tmp/i128.err" || {
    echo "__int128 failed for the wrong reason:"; cat "$tmp/i128.err"; exit 1; }
"$EMBCC" --target="$R64" --dump-predef | grep -q '^#define __SIZEOF_INT128__ 16' || {
    echo "RV64: the table does not advertise __int128"; exit 1; }
"$EMBCC" --target="$R32" --dump-predef | grep -q '^#define __SIZEOF_INT128__' && {
    echo "RV32: the table advertises an __int128 the front end refuses"; exit 1; }
echo "__int128 is present at RV64, refused by name at RV32, and the table agrees"

# 6. THE RULE. There is no code generator, so -c must say so and write
#    nothing. Handing the unit to the x86-64 backend because it is
#    "not aarch64 and not thumb" is the failure this guards.
printf 'int add(int a, int b) { return a + b; }\n' > "$tmp/add.c"
for t in "$R32" "$R64"; do
    rm -f "$tmp/add.o"
    if "$EMBCC" --target="$t" -c "$tmp/add.c" -o "$tmp/add.o" 2>"$tmp/cg.err"; then
        echo "$t: -c produced an object and there is no backend for it"; exit 1
    fi
    grep -q "has no code generator yet" "$tmp/cg.err" || {
        echo "$t: -c failed for the wrong reason:"; cat "$tmp/cg.err"; exit 1; }
    grep -q -- "--target=$t" "$tmp/cg.err" || {
        echo "$t: the refusal does not name the target:"; cat "$tmp/cg.err"; exit 1; }
    [ ! -s "$tmp/add.o" ] || {
        echo "$t: -c refused and left an object behind anyway"; exit 1; }
done
echo "-c refuses by name on both widths and writes nothing"

# 7. What DOES work today works.
for t in "$R32" "$R64"; do
    "$EMBCC" --target="$t" -E "$tmp/add.c" > /dev/null || {
        echo "$t: -E does not work"; exit 1; }
    "$EMBCC" --target="$t" -fsyntax-only "$tmp/add.c" || {
        echo "$t: -fsyntax-only does not work"; exit 1; }
done
echo "-E and -fsyntax-only work for both RISC-V widths"
