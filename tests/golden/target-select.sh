#!/bin/sh
# make TARGETS=: a compiler built for the targets a person uses, as GCC is
# configured for one -- not every backend and every runtime whether wanted
# or not.
#
# A compiler built with `make TARGETS=arm` is a different binary from the
# full one, and the claims worth a test are what tell the two apart:
#
#  1. THE OTHER BACKENDS ARE GONE. Not hidden behind a check: their code
#     is not in the binary, which is the point (the size, the build time).
#  2. WHAT IS LEFT IS UNCHANGED. Every place shared code reaches into a
#     backend is under FAMILY_BUILT (src/arch/target.h); a guard put one
#     line too wide changes what the remaining backend emits, so its
#     objects are compared with the full compiler's, byte for byte.
#  3. A TARGET IT LACKS IS REFUSED BY NAME, with what it has and the
#     TARGETS= that adds the missing family -- before anything asks for a
#     backend that is not there.
#  4. THE BUILD FOLLOWS: its default target is its own family's, the
#     runtimes and the install are only its triples, an unknown family
#     stops make, and tests/run.sh skips a suite the compiler has no
#     backend for instead of failing it test by test.
#
# EMBCC_TARGETS_EVERY=1 builds a one-family compiler for each of the 13
# families as well (a few minutes): a new reference from shared code into
# one backend fails exactly the builds without it, and the default run
# builds only one.
set -u
echo "TEST-MARKER target-select"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/target-select
rm -rf "${out:?}"; mkdir -p "$out"

fail() { echo "FAIL: $*"; exit 1; }

# ---- the build ---------------------------------------------------------
# Into a BUILD of its own and linked there ($(BUILD)/embcc): this runs
# beside every other test, which are using ./embcc. DEFAULT_TARGET= in
# case the tree was ./configure'd with one.
make -C "$EMBCC_ROOT" -j4 BUILD="$out/b" TARGETS=arm DEFAULT_TARGET= \
    "$out/b/embcc" > "$out/build.log" 2>&1 || {
    tail -20 "$out/build.log"; fail "make TARGETS=arm did not build"; }
# The objects are the subset's, in their own directory.
[ -d "$out/b/targets-arm" ] ||
    fail "a TARGETS=arm build put its objects elsewhere than BUILD/targets-arm"

# A tree for it: the binary beside the source tree's lib/, include/ and
# build/, so it finds the same headers and runtimes the full one does
# (src/driver/paths.c, "a build tree").
t=$out/tree
mkdir -p "$t"
for d in lib include build; do ln -s "$EMBCC_ROOT/$d" "$t/$d"; done
cp "$out/b/embcc" "$t/embcc"
sub=$t/embcc

# ---- 1. the other backends are not in it -------------------------------
nm "$sub" > "$out/nm.txt" 2>/dev/null || fail "nm could not read $sub"
grep -q 'codegen_unit_thumb$' "$out/nm.txt" ||
    fail "the ARM code generator is not in a TARGETS=arm compiler"
for gone in codegen_unit_riscv codegen_unit_avr codegen_unit_mips \
            codegen_unit_arm64 rvasm_assemble irg_va_arg_sysv \
            predef_macros_riscv32 rv_enc_b; do
    if grep -q "$gone\$" "$out/nm.txt"; then
        fail "$gone is in a TARGETS=arm compiler"
    fi
done
# x86-64's code generator is the one every other backend used to borrow
# helpers from (now src/arch/common.c): it is gone too.
grep -q ' _*codegen_unit$' "$out/nm.txt" &&
    fail "the x86-64 code generator is in a TARGETS=arm compiler"
full_size=$(wc -c < "$EMBCC" | tr -d " ")
sub_size=$(wc -c < "$sub" | tr -d " ")
echo "  TARGETS=arm: $sub_size bytes; the full compiler: $full_size"

# ---- 2. what is left emits what it did ---------------------------------
cat > "$out/k.c" << 'EOF'
#include <stdarg.h>
#include <string.h>
struct pt { int x, y; };
static int sum(int n, ...)
{
    va_list ap;
    int s = 0;
    va_start(ap, n);
    while (n--)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}
long long mix(long long a, unsigned b) { return (a << 3) ^ (a >> 7) ^ b; }
int run(struct pt *p, int k)
{
    char buf[16];
    memset(buf, k, sizeof buf);
    if (p->x > 0x12345)
        __asm__ volatile ("nop");
    return sum(3, p->x, p->y, buf[5]) + (int)mix(k, 0x8000u);
}
EOF
n=0
for tri in thumbv7m-none-eabi thumbv6m-none-eabi thumbv7em-none-eabihf \
           thumbv8m.main-none-eabi armv7a-none-eabi; do
    for o in -O0 -O2 -Os; do
        "$EMBCC" --target=$tri $o -c "$out/k.c" -o "$out/full.o" ||
            fail "the full compiler did not compile for $tri $o"
        "$sub" --target=$tri $o -c "$out/k.c" -o "$out/sub.o" ||
            fail "the TARGETS=arm compiler did not compile for $tri $o"
        cmp -s "$out/full.o" "$out/sub.o" ||
            fail "$tri $o: the TARGETS=arm compiler's object differs from the full one's"
        n=$((n + 1))
    done
done
# ...and the corpus, at one level, for the base Cortex-M target
for c in "$EMBCC_ROOT"/tests/exec/*.c; do
    "$EMBCC" --target=thumbv7m-none-eabi -O2 -c "$c" -o "$out/full.o" \
        2>/dev/null || continue
    "$sub" --target=thumbv7m-none-eabi -O2 -c "$c" -o "$out/sub.o" \
        2>/dev/null ||
        fail "$(basename "$c"): the full compiler compiles it, TARGETS=arm does not"
    cmp -s "$out/full.o" "$out/sub.o" ||
        fail "$(basename "$c"): the TARGETS=arm compiler's object differs"
    n=$((n + 1))
done
echo "  $n objects byte-identical to the full compiler's"
# the predefined macros are a table per target: the same ones
for x in c c++; do
    "$EMBCC" --target=thumbv8m.main-none-eabi -x $x --dump-predef > "$out/p1"
    "$sub" --target=thumbv8m.main-none-eabi -x $x --dump-predef > "$out/p2"
    cmp -s "$out/p1" "$out/p2" || fail "--dump-predef ($x) differs"
done

# ---- 3. a target it lacks is refused by name ---------------------------
if "$sub" --target=riscv32-unknown-elf -c "$out/k.c" -o "$out/rv.o" \
        2> "$out/rv.err"; then
    fail "the TARGETS=arm compiler compiled for riscv32-unknown-elf"
fi
cat "$out/rv.err"
grep -q "error: this embcc was built without the riscv backend" "$out/rv.err" ||
    fail "the refusal does not name the riscv backend"
grep -q 'make TARGETS="arm riscv"' "$out/rv.err" ||
    fail "the refusal does not say which TARGETS= adds riscv"
[ -f "$out/rv.o" ] && fail "the refused compile left an object"
# every way of naming a target arrives at the same refusal
EMBCC_DEFAULT_TARGET=avr "$sub" -c "$out/k.c" -o "$out/x.o" 2> "$out/def.err" &&
    fail "an EMBCC_DEFAULT_TARGET it lacks was not refused"
grep -q "without the avr backend" "$out/def.err" &&
grep -q "EMBCC_DEFAULT_TARGET" "$out/def.err" ||
    fail "a default target it lacks is not refused saying where it came from"
"$sub" -arch x86_64 -c "$out/k.c" -o "$out/x.o" 2> "$out/arch.err" &&
    fail "-arch x86_64 was not refused"
grep -q "without the x86 backend" "$out/arch.err" ||
    fail "-arch x86_64 is not refused by family"
ln -s embcc "$t/riscv64-unknown-elf-gcc"
"$t/riscv64-unknown-elf-gcc" -c "$out/k.c" -o "$out/x.o" 2> "$out/name.err" &&
    fail "called as riscv64-unknown-elf-gcc, it compiled"
grep -q "without the riscv backend" "$out/name.err" ||
    fail "called by a riscv name, it is not refused by family"
# the linker likewise refuses a machine it has no backend for
"$EMBCC" --target=riscv32-unknown-elf -c "$out/k.c" -o "$out/rv.o" ||
    fail "the full compiler did not compile for riscv32"
if "$sub" "$out/rv.o" -nostdlib --linker-arg=-Ttext=0 \
        --linker-arg=-Tdata=0x20000000 -o "$out/rv.elf" 2> "$out/ld.err"; then
    fail "the TARGETS=arm compiler linked a RISC-V object"
fi
grep -q "an object for the riscv family, and this compiler was built without it" \
    "$out/ld.err" ||
    { cat "$out/ld.err"; fail "the link of a RISC-V object is not refused naming the family"; }
echo "  a target it lacks is refused, by --target=, the default, -arch and the name"

# ---- 4. the build around it --------------------------------------------
v=$("$sub" --version) || fail "--version failed"
echo "$v"
echo "$v" | grep -q "^  arm  " || fail "--version does not list arm"
echo "$v" | grep -q "^  riscv " && fail "--version lists riscv"
echo "$v" | grep -q "^Not built:.* riscv" || fail "--version does not say riscv is not built"
echo "$v" | grep -q "^Target: thumbv7m-none-eabi$" ||
    fail "a compiler without x86 does not default to its own family's target"
"$EMBCC" --version | grep -q "^Not built" &&
    fail "the full compiler's --version says something is not built"

# runtimes and install: the family's triples, no other
make -C "$EMBCC_ROOT" -n BUILD="$out/b" TARGETS=arm runtimes install-files \
    DESTDIR="$out/stage" > "$out/rt.txt" 2>&1 ||
    { cat "$out/rt.txt"; fail "make -n TARGETS=arm runtimes failed"; }
# the triples are the lists the loops of rt-, libc-, libcxx-embedded and
# install-files walk (the recipes' own text names every target's case)
grep -A1 -E '^[[:space:]]*for (t|pair) in' "$out/rt.txt" > "$out/lists.txt"
for want in thumbv7m-none-eabi thumbv6m-none-eabi armv7a-none-eabihf; do
    grep -q "$want" "$out/lists.txt" ||
        fail "TARGETS=arm does not build $want's runtime"
done
for gone in riscv32-unknown-elf '"avr:avr"' ' avr ' x86_64-elf \
            aarch64-linux-gnu xtensa-none-elf m68k-none-elf; do
    grep -q "$gone" "$out/lists.txt" && fail "TARGETS=arm builds or installs $gone"
done
# ...and no x86-64 or AArch64 library is built at all
for gone in libc/x86_64 libcxx/x86_64 libc/linux-aarch64 libc/aarch64; do
    grep -q "$gone" "$out/rt.txt" && fail "TARGETS=arm builds $gone"
done
# an unknown family stops make, naming the ones there are
make -C "$EMBCC_ROOT" -n TARGETS=arm,sparkle embcc > "$out/bad.txt" 2>&1 &&
    fail "make TARGETS=sparkle did not stop"
grep -q "no target family 'sparkle'" "$out/bad.txt" ||
    { cat "$out/bad.txt"; fail "an unknown family is not refused by name"; }
# a backend directory's name is its family's
make -C "$EMBCC_ROOT" -pn TARGETS=thumb,coldfire 2>/dev/null |
    grep -q '^TARGETS_BUILT := arm m68k$' ||
    fail "TARGETS=thumb,coldfire is not arm and m68k"
# ./configure asks the Makefile, refuses the same way, and writes nothing
# then (its success writes config.mk in the tree, which every other test's
# make would read: not tried here)
before=$(cksum "$EMBCC_ROOT/config.mk" 2>/dev/null || echo none)
"$EMBCC_ROOT/configure" --targets=arm,sparkle > "$out/conf.txt" 2>&1 &&
    fail "./configure --targets=arm,sparkle succeeded"
grep -q "no target family 'sparkle'" "$out/conf.txt" ||
    { cat "$out/conf.txt"; fail "./configure does not name the unknown family"; }
[ "$(cksum "$EMBCC_ROOT/config.mk" 2>/dev/null || echo none)" = "$before" ] ||
    fail "a refused ./configure changed config.mk"

# tests/run.sh: no x86 here, so the suite is skipped, cleanly
if ! sh "$EMBCC_ROOT/tests/run.sh" --embcc="$sub" --exec-only \
        > "$out/run.txt" 2>&1; then
    cat "$out/run.txt"
    fail "tests/run.sh failed for a compiler without x86 instead of skipping"
fi
grep -q "^run.sh: skipped: this embcc was built without the x86 backend" \
    "$out/run.txt" || { cat "$out/run.txt"; fail "run.sh did not say why it skipped"; }
echo "  $(head -1 "$out/run.txt")"

# ---- every family alone (opt-in) ---------------------------------------
if [ "${EMBCC_TARGETS_EVERY:-0}" = 1 ]; then
    for f in x86 aarch64 arm riscv avr mips loongarch tricore xtensa ppc rx \
             sparc m68k; do
        make -C "$EMBCC_ROOT" -j4 BUILD="$out/every" TARGETS=$f \
            "$out/every/embcc" > "$out/every-$f.log" 2>&1 || {
            tail -20 "$out/every-$f.log"; fail "make TARGETS=$f did not build"; }
        echo "  TARGETS=$f: $(wc -c < "$out/every/embcc") bytes"
        rm -f "$out/every/embcc"
    done
fi
echo "PASS: a TARGETS=arm compiler has ARM alone, emits what the full one does, and refuses the rest by name"
