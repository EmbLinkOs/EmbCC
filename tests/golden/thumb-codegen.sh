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
    "$EMBCC" --target=$T $opt -c tests/golden/embedded-stress.c \
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
refuses "a long double"     'long double f(long double a){return a*a;}'
echo "long double refuses by name"

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

# Floating point is a CALL on this machine, not an instruction: every
# operation goes to lib/rt/softfp.c under libgcc's names.
printf '%s\n' 'double f(double a, double b){ return a*b + a/b - a; }
float g(float a, int n){ return a * (float)n; }
int h(double a, double b){ return a < b; }' > "$out/fp.c"
"$EMBCC" --target=$T -O1 -c "$out/fp.c" -o "$out/fp.o" || {
    echo "floating point no longer compiles"; exit 1; }
"$RE" -r "$out/fp.o" > "$out/fprel.txt"
for h in __muldf3 __divdf3 __subdf3 __mulsf3 __ltdf2 __floatsisf; do
    grep -q "$h" "$out/fprel.txt" || {
        echo "expected a call to $h and found none:"; cat "$out/fprel.txt"
        exit 1; }
done
echo "floating point lowers to the soft-float runtime"

# Aggregates by value, in every shape AAPCS32 treats differently.
printf '%s\n' 'struct s1{char a;}; struct s8{int a,b;}; struct s20{int v[5];};
struct s1 a(struct s1 x){ return x; }
struct s8 b(struct s8 x){ return x; }
struct s20 c(struct s20 x){ return x; }
int d(int p,int q,int r, struct s8 s){ return p+q+r+s.a+s.b; }' > "$out/ag.c"
"$EMBCC" --target=$T -O1 -c "$out/ag.c" -o "$out/ag.o" || {
    echo "aggregates by value no longer compile"; exit 1; }
echo "aggregates by value compile"

# Variadic functions: the prologue spills r0-r3 below the caller's
# stack arguments so one pointer walks from the registers into them.
printf '%s\n' '#include <stdarg.h>
int f(int n, ...){ va_list ap; int t=0; va_start(ap,n);
  for(int i=0;i<n;i++) t+=va_arg(ap,int); va_end(ap); return t; }
double g(int n, ...){ va_list ap; va_start(ap,n);
  double d = va_arg(ap,double); va_end(ap); return d; }' > "$out/va.c"
"$EMBCC" --target=$T -O1 -I include -c "$out/va.c" -o "$out/va.o" || {
    echo "variadic functions no longer compile"; exit 1; }
echo "variadic functions compile"

# ---- what the allocator is allowed to keep in a register --------------
#
# Three things used to force values into stack slots on this target, and
# each cost instructions in EVERY function:
#
#   - ret_scalar_in_reg was 0, so a returned value went out through its
#     slot: `str r12, [sp,#N]` then `ldr r0, [sp,#N]`, always.
#   - there were no ABI hints, so the value the allocator chose was
#     rarely the register the ABI wanted, and the move stayed.
#   - layout() reserved a stack slot for EVERY vreg, including the ones
#     the allocator had just put in registers -- paid for in `sub sp`
#     and then never read or written.
#
# The test is on the emitted code rather than on a byte count, because a
# count moves with unrelated changes and says nothing about why.
cat > "$out/q.c" <<'EOF'
int  add32(int a, int b)        { return a + b; }
int  chain(int a, int b, int c) { int x = a * b; return x + c; }
int  ld(const int *p)           { return p[3]; }
EOF
"$EMBCC" --target=$T -Os -c "$out/q.c" -o "$out/q.o" || {
    echo "the register-quality file does not compile"; exit 1; }
"$OD" -d --triple=thumbv7m --no-show-raw-insn "$out/q.o" > "$out/q.s" 2>&1

# None of these three touches memory at all: every value is a register,
# so a load or store from sp means something went back to a slot.
n=$(grep -cE '(ldr|str)[a-z.]*[[:space:]].*\[sp' "$out/q.s" || true)
[ "$n" = 0 ] || {
    echo "a value round-tripped through a stack slot in a function that
needs none ($n access(es)):"
    cat "$out/q.s"; exit 1; }
# ...and the result is computed straight into r0, not moved there.
grep -qE '^ *[0-9a-f]+:[[:space:]]+add(\.w|s)?[[:space:]]+r0,' "$out/q.s" || {
    echo "the returned sum does not land in r0:"; cat "$out/q.s"; exit 1; }
echo "a function whose values all fit in registers touches no stack slot,
and its result is computed straight into r0"

# The two cases where a slot is still REQUIRED, so the skip above cannot
# be doing it by forgetting.
printf 'int g(int *);\nint f(int a){ int x = a + 1; return g(&x) + x; }\n' \
    > "$out/at.c"
"$EMBCC" --target=$T -Os -c "$out/at.c" -o "$out/at.o" || {
    echo "the address-taken file does not compile"; exit 1; }
"$OD" -d --triple=thumbv7m --no-show-raw-insn "$out/at.o" > "$out/at.s" 2>&1
grep -qE '(ldr|str)[a-z.]*[[:space:]].*\[sp' "$out/at.s" || {
    echo "a local whose ADDRESS is taken lost its stack slot:"
    cat "$out/at.s"; exit 1; }
echo "a local whose address is taken keeps its slot"

# Under -g every local is pinned to a slot, which is what makes
# DW_AT_location naming that slot true. If the skip ever fires here, the
# debugger is told where a variable is not.
if command -v llvm-dwarfdump > /dev/null 2>&1; then
    printf 'int f(int a, int b){ int s = a + b; return s * 2; }\n' \
        > "$out/g.c"
    "$EMBCC" --target=$T -Os -g -c "$out/g.c" -o "$out/g.o" || {
        echo "the -g file does not compile"; exit 1; }
    llvm-dwarfdump "$out/g.o" > "$out/g.dw" 2>&1
    for v in a b s; do
        grep -A3 "DW_AT_name	(\"$v\")" "$out/g.dw" |
            grep -q 'DW_AT_location.*DW_OP_fbreg' || {
            echo "-g: '$v' has no frame location, so the skip fired under -g"
            exit 1; }
    done
    echo "-g pins every local to a slot, and each has a DW_OP_fbreg location"
else
    echo "SKIP the -g half: no llvm-dwarfdump"
fi

# ---- the push keeps sp eight-byte aligned -----------------------------
#
# AAPCS32 requires sp to be eight-byte aligned at every public
# interface. SAVE_MASK is four registers, chosen even for exactly that
# reason -- but the allocator's callee-saved set is pushed by the SAME
# instruction, and its parity was never counted. An odd number of extra
# registers makes the push 4 mod 8, and every eight-byte object below it
# is then four bytes out.
#
# That was reachable before anything in this file changed and became
# common once call arguments started living in registers: `main` in
# embedded-varargs.c went from pushing four registers to nine, and the
# variadic callee read its eight-byte stack arguments from a misaligned
# frame. The symptom was a `long long` argument reading as zero, three
# calls away from the function with the wrong prologue.
#
# So the invariant is asserted directly, over a range of functions
# chosen to need different numbers of registers -- an assertion on the
# push itself does not depend on some caller happening to notice.
cat > "$out/al.c" <<'EOF'
int f1(int a) { return a; }
int f2(int a, int b) { return a * b; }
int f3(int a, int b, int c) { int x = a * b, y = b * c; return x + y + a; }
int f4(int a, int b, int c, int d)
{ int p = a * b, q = c * d, r = p + q, s = p * q; return p + q + r + s + a; }
long long f5(long long a, long long b) { return a * b + a; }
int f6(const int *p, int n)
{ int s = 0, t = 1, u = 2, v = 3;
  for (int i = 0; i < n; i++) { s += p[i]; t ^= p[i]; u += t; v *= 3; }
  return s + t + u + v; }
int g(int, int, int, int, int, int);
int f7(int a, int b, int c, int d, int e, int f)
{ return g(f, e, d, c, b, a) + g(a, b, c, d, e, f); }
EOF
"$EMBCC" --target=$T -Os -c "$out/al.c" -o "$out/al.o" || {
    echo "the alignment file does not compile"; exit 1; }
"$OD" -d --triple=thumbv7m --no-show-raw-insn "$out/al.o" > "$out/al.s" 2>&1
# Every `push` in the object, counted. A push moves sp by 4 per
# register, so an odd count leaves it 4 mod 8.
bad=0
while IFS= read -r line; do
    regs=$(printf '%s\n' "$line" | sed 's/.*{//; s/}.*//')
    n=$(printf '%s\n' "$regs" | tr ',' '\n' | grep -c '[a-z]')
    if [ $((n % 2)) != 0 ]; then
        echo "a push of $n registers leaves sp 4 mod 8: $line"
        bad=1
    fi
done <<EOF2
$(grep -E '^\s*[0-9a-f]+:\s+push' "$out/al.s")
EOF2
[ "$bad" = 0 ] || exit 1
np=$(grep -cE '^\s*[0-9a-f]+:\s+push' "$out/al.s" || true)
[ "$np" -ge 4 ] || {
    echo "only $np push(es) in the alignment file -- it no longer
exercises a range of register counts"; exit 1; }
echo "all $np prologue pushes move sp by a multiple of eight"
