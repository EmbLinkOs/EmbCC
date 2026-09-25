#!/bin/sh
# What the ARMv7-M backend emits (D-015): a real ELF32 ARM object whose
# every instruction decodes, whose relocations are the ARM ones, and
# whose refusals fire by name.
#
# There is no linker for this target here yet, so nothing is RUN. What
# can be checked without one is checked: that the object is the shape an
# ARM toolchain expects, that no byte of .text disassembles as
# <unknown> — which is what a wrong encoding looks like — and that
# everything the backend does not implement stops rather than emitting
# something plausible.
set -u
echo "TEST-MARKER thumb-codegen"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
command -v "$OD" >/dev/null 2>&1 && command -v "$RE" >/dev/null 2>&1 || {
    echo "SKIP: llvm-objdump/llvm-readelf not found"; exit 0; }

T=thumbv7m-none-eabi
out=tests/golden/out/thumb-codegen
rm -rf "$out"; mkdir -p "$out"

cat > "$out/prog.c" <<'EOF'
static int table[8];
const char *msg = "hello";
int gsum;

int sum(const int *p, int n)
{
    int t = 0;
    for (int i = 0; i < n; i++)
        t += p[i];
    return t;
}

int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }

void fill(int n)
{
    for (int i = 0; i < 8; i++)
        table[i] = i * n;
}

unsigned char pack(int a, int b) { return (unsigned char)(a * 3 + b / 2); }

short widen(signed char c) { return (short)(c << 4); }

int strlen_(const char *s) { int n = 0; while (s[n]) n++; return n; }

int choose(int a, int b, int c, int d, int e, int f)
{
    return a > b ? c + d : e - f;     /* six arguments: r0-r3 then stack */
}

int main(void)
{
    fill(3);
    gsum = sum(table, 8) + fact(5) + strlen_(msg);
    return gsum + pack(2, 8) + widen(-3) + choose(1, 2, 3, 4, 5, 6);
}
EOF

for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c "$out/prog.c" -o "$out/prog$opt.o" || {
        echo "$opt: the backend could not compile the sample"; exit 1; }

    # Every instruction must decode. An encoding with a wrong bit shows
    # up here and nowhere else until the code runs.
    "$OD" -d --triple=thumbv7m "$out/prog$opt.o" > "$out/dis$opt.txt" || {
        echo "$opt: llvm-objdump could not read the object"; exit 1; }
    bad=$(grep -c "unknown\|<invalid>" "$out/dis$opt.txt" || true)
    [ "$bad" = 0 ] || {
        echo "$opt: $bad instructions do not decode:"
        grep -n "unknown\|<invalid>" "$out/dis$opt.txt" | head -5
        exit 1; }
done
echo "the sample compiles at four optimisation levels and every instruction decodes"

# The wider program: structs, two-dimensional arrays, switch, recursion,
# a function pointer, do/while with break and continue, bit counting,
# signed and unsigned division and modulo, and string traversal. Its
# OUTPUT was checked against clang's for the same source, running both on
# QEMU's Cortex-M3 — which is where the opcode table that turned `and`
# into `eor` was caught. Without a linker in the tree that comparison
# cannot run here yet, so what this checks is that it still compiles and
# still decodes.
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c tests/golden/thumb-stress.c \
             -o "$out/stress$opt.o" || {
        echo "$opt: the stress program does not compile"; exit 1; }
    "$OD" -d --triple=thumbv7m "$out/stress$opt.o" > "$out/sdis$opt.txt"
    bad=$(grep -c "unknown\|<invalid>" "$out/sdis$opt.txt" || true)
    [ "$bad" = 0 ] || {
        echo "$opt: $bad instructions of the stress program do not decode"
        exit 1; }
done
echo "the stress program compiles and decodes at four levels too"

# The object's shape, which is what a linker checks before anything else.
"$RE" -h "$out/prog-O2.o" > "$out/hdr.txt"
grep -q "Class:  *ELF32" "$out/hdr.txt" || {
    echo "the object is not ELF32"; cat "$out/hdr.txt"; exit 1; }
grep -q "Machine:  *ARM" "$out/hdr.txt" || {
    echo "e_machine is not EM_ARM"; exit 1; }
grep -q "Flags:  *0x5000000" "$out/hdr.txt" || {
    echo "e_flags is not EF_ARM_EABI_VER5 — an ARM consumer reads this"
    grep Flags "$out/hdr.txt"; exit 1; }
echo "ELF32, EM_ARM, EABI version 5"

# A Thumb function symbol carries bit 0 SET. Without it a `blx` through a
# function pointer switches to ARM state and the processor faults.
"$RE" -s "$out/prog-O2.o" > "$out/syms.txt"
# The last hex digit of st_value is all this needs, and asking for it
# that way keeps the test in POSIX awk — strtonum() is a gawk extension
# and silently prints nothing on the BSD awk macOS ships, which is a
# check that passes whatever the object says.
odd=$(awk '$4 == "FUNC" {
               d = substr($2, length($2), 1)
               if (index("13579bdfBDF", d) == 0) print $8
           }' "$out/syms.txt")
[ -z "$odd" ] || {
    echo "these Thumb functions have an even st_value (no Thumb bit): $odd"
    exit 1; }
grep -q '\$t' "$out/syms.txt" || {
    echo "no \$t mapping symbol: a disassembler will read .text as ARM"
    exit 1; }
echo "every function symbol carries the Thumb bit, and \$t marks the section"

# The relocations are ARM's, and the pair that takes an address is a pair.
"$RE" -r "$out/prog-O2.o" > "$out/rel.txt"
for r in R_ARM_THM_MOVW_ABS_NC R_ARM_THM_MOVT_ABS R_ARM_ABS32; do
    grep -q "$r" "$out/rel.txt" || {
        echo "expected a $r relocation and found none"; cat "$out/rel.txt"
        exit 1; }
done
w=$(grep -c R_ARM_THM_MOVW_ABS_NC "$out/rel.txt" || true)
t=$(grep -c R_ARM_THM_MOVT_ABS "$out/rel.txt" || true)
[ "$w" = "$t" ] || {
    echo "$w movw relocations against $t movt: an address needs both halves"
    exit 1; }
echo "ARM relocations, $w movw/movt pairs and an ABS32 in .data"

# THE RULE: what the backend has not got, it refuses by name.
refuses() {
    printf '%s\n' "$2" > "$out/no.c"
    if "$EMBCC" --target=$T -c "$out/no.c" -o "$out/no.o" 2>"$out/no.err"; then
        echo "$1 was accepted by a backend that cannot lower it"; exit 1
    fi
    grep -q "cannot lower" "$out/no.err" || {
        echo "$1 failed, but not with the backend's own refusal:"
        cat "$out/no.err"; exit 1; }
}
refuses "floating point"    'double f(double a, double b){return a*b;}'
refuses "a variadic function" '
#include <stdarg.h>
int f(int n, ...){ va_list ap; va_start(ap, n); return n; }'
refuses "an aggregate argument" '
struct big { int a, b, c; };
int g(struct big);
int f(struct big b){ return g(b); }'
echo "floating point, varargs and aggregates each refuse by name"

# And what it NO LONGER refuses: 64-bit integers, which the backend
# carries in register pairs. Checked here as well as in thumb-exec.sh
# so the compile path is covered even where QEMU is not installed.
printf '%s\n' 'long long f(long long a, long long b){ return a*b - (a>>3) + (a<b); }
unsigned long long g(unsigned long long a){ return a / 1000ULL; }' > "$out/ll.c"
for opt in -O0 -O2; do
    "$EMBCC" --target=$T $opt -c "$out/ll.c" -o "$out/ll.o" || {
        echo "$opt: 64-bit arithmetic no longer compiles"; exit 1; }
    "$OD" -d --triple=thumbv7m "$out/ll.o" > "$out/lldis.txt"
    grep -q "$out/lldis.txt" -e unknown && {
        echo "$opt: a 64-bit lowering does not decode"; exit 1; }
done
echo "64-bit integers compile at -O0 and -O2"
