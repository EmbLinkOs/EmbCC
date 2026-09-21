#!/bin/sh
# The Microsoft x64 calling convention, refereed by clang.
#
# D-014 called this "the part with no precedent in the tree": aarch64's
# AAPCS64 arrived WITH a new backend, so "one architecture, one
# convention" held. Win64 is a different convention on x86-64, and the
# thing that makes it dangerous is that a wrong implementation produces
# CORRECT code for every argument list that happens to be all one
# class. f(int, int, int) is right under either reading; f(int, double,
# int) is where they part.
#
# So this does not check EmbCC against itself. clang can target
# x86_64-windows-gnu, so the same source goes through both and the
# register each argument lands in is compared. What is asserted is the
# ABI, not the code: clang's scheduling, its constant materialisation
# and its choice of 32- or 64-bit moves are all its own business.
set -eu
echo "TEST-MARKER win-abi"
. "$(dirname "$0")/../lib.sh"

[ "$ARCH" = x86_64 ] || { echo "skipped: Win64 is an x86-64 convention"
                          exit 0; }
command -v clang > /dev/null 2>&1 || {
    echo "skipped: no clang to referee the convention against"; exit 0; }
clang --target=x86_64-windows-gnu -S -o /dev/null -xc /dev/null \
    > /dev/null 2>&1 || {
    echo "skipped: this clang cannot target x86_64-windows-gnu"; exit 0; }
OD=x86_64-elf-objdump
command -v "$OD" > /dev/null 2>&1 || { echo "skipped: no $OD"; exit 0; }

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/win-abi
rm -rf "$out"; mkdir -p "$out"

# embcc_asm SOURCE -> $out/e.txt, the disassembly of what EmbCC emitted
embcc_asm() {
    "$EMBCC" --target=x86_64-windows-gnu -O1 -c "$1" -o "$out/e.obj" \
        2> "$out/cc.log" || {
        echo "FAIL: embcc could not compile:"; cat "$out/cc.log"; exit 1; }
    "$OD" -d "$out/e.obj" > "$out/e.txt" 2>&1
}

# want FUNCTION REGEX WHAT -- the regex must match inside that function
want() {
    sed -n "/<$1>:/,/ret/p" "$out/e.txt" > "$out/fn.txt"
    grep -qE "$2" "$out/fn.txt" || {
        echo "FAIL: $1 does not $3"
        echo "      (looked for /$2/)"
        cat "$out/fn.txt"; exit 1; }
}

# ---- 1. the shared slot, on both sides ------------------------------------
#
# The whole convention in one line: an argument's register comes from
# its POSITION, so a double in slot 1 is xmm1 while an integer in slot 1
# is rdx. A per-class counter would put the double in xmm0 and the
# third argument in rdx, and every all-integer call would still work.
cat > "$out/mix.c" << 'EOF'
int f(int a, double b, int c, double d, int e);
int caller(void) { return f(1, 2.0, 3, 4.0, 5); }
int callee(int a, double b, int c, double d, int e)
{ return a + c + e + (int)b + (int)d; }
EOF
embcc_asm "$out/mix.c"

#    What clang does with the same source, read rather than assumed.
clang --target=x86_64-windows-gnu -O1 -S -o "$out/clang.s" "$out/mix.c" \
    2> "$out/clang.log" || {
    echo "FAIL: clang could not compile the reference:"
    cat "$out/clang.log"; exit 1; }
for r in ecx xmm1 r8d xmm3; do
    grep -q "%$r" "$out/clang.s" || {
        echo "FAIL: the reference does not use %$r, so this test has the"
        echo "      convention wrong, not EmbCC:"; cat "$out/clang.s"
        exit 1; }
done

want caller 'mov .*%rcx'       'put the first integer in rcx'
want caller 'movsd .*%xmm1'    'put the second argument in xmm1 (slot 1)'
want caller 'mov .*%r8'        'put the third integer in r8 (slot 2)'
want caller 'movsd .*%xmm3'    'put the fourth argument in xmm3 (slot 3)'
want caller 'mov .*0x20\(%rsp\)' \
     'place the fifth argument above the 32 bytes of shadow space'

want callee 'mov .*%rcx,'      'read the first integer from rcx'
want callee 'movsd .*%xmm1,'   'read the second argument from xmm1'
want callee 'mov .*%r8,'       'read the third integer from r8'
want callee 'movsd .*%xmm3,'   'read the fourth argument from xmm3'
want callee 'mov .*0x30\(%rbp\)' \
     'read the fifth argument from above its own shadow space'
echo "the slot is the argument's position on both sides: rcx, xmm1, r8,
xmm3, and the fifth above 32 bytes of shadow space -- clang agrees"

# ---- 2. shadow space is owed even when nothing goes on the stack ----------
#
# A callee may spill its four register arguments into the 32 bytes below
# the return address without asking, so a caller that did not reserve
# them has its own locals overwritten. Nothing in a two-argument call
# hints that the space is needed, which is why this is its own check.
cat > "$out/shadow.c" << 'EOF'
int two(int a, int b);
int g(int x) { return two(x, x); }
EOF
embcc_asm "$out/shadow.c"
sub=$(sed -n '/<g>:/,/ret/p' "$out/e.txt" | \
      sed -n 's/.*sub *\$0x\([0-9a-f]*\),%rsp.*/\1/p' | head -1)
[ -n "$sub" ] || { echo "FAIL: no frame allocation in g:"
                   sed -n '/<g>:/,/ret/p' "$out/e.txt"; exit 1; }
[ "$(printf '%d' "0x$sub")" -ge 32 ] || {
    echo "FAIL: g reserves only $((0x$sub)) bytes, and owes its callee 32"
    sed -n '/<g>:/,/ret/p' "$out/e.txt"; exit 1; }
echo "a call with two arguments still reserves the 32 bytes its callee
is allowed to spill into"

# ---- 3. a variadic float goes in BOTH registers --------------------------
#
# The callee has no prototype, so it does not know whether to read the
# xmm or the integer register -- the caller fills both. Checked against
# clang, which emits the same movq from xmm to the integer slot.
cat > "$out/va.c" << 'EOF'
int p(const char *, ...);
int v(void) { return p("x", 1, 2.5, 3); }
EOF
embcc_asm "$out/va.c"
clang --target=x86_64-windows-gnu -O1 -S -o "$out/cva.s" "$out/va.c" \
    > /dev/null 2>&1 || true
#  A tab, not spaces, is what the assembler output puts between the
#  mnemonic and its operands.
grep -qE 'movq[[:space:]]+%xmm[0-9]+,[[:space:]]*%r' "$out/cva.s" || {
    echo "FAIL: the reference does not copy a variadic double into an"
    echo "      integer register, so this check has it wrong:"
    cat "$out/cva.s"; exit 1; }
want v 'movq[[:space:]]+%xmm2,%r8|mov[[:space:]].*%r8' \
     'give the variadic double its integer register as well as xmm2'
echo "a variadic double is placed in its xmm register and in the integer
register of the same slot, as the reference does"

# ---- 4. what is refused rather than passed the System V way --------------
#
# Each of these has a Microsoft x64 answer EmbCC does not implement, and
# the System V answer is not it. Refusing names the thing; passing it
# would link and misbehave at the first call.
refuses() {                       # refuses SOURCE EXPECTED
    printf '%s\n' "$1" > "$out/r.c"
    if "$EMBCC" --target=x86_64-windows-gnu -c "$out/r.c" -o /dev/null \
           2> "$out/r.log"; then
        echo "FAIL: accepted something it cannot pass correctly:"
        echo "      $1"; exit 1
    fi
    grep -q "$2" "$out/r.log" || {
        echo "FAIL: the refusal does not say what is wrong:"
        cat "$out/r.log"; exit 1; }
}
refuses 'struct s { int a, b, c; }; int t(struct s); int g(struct s v){ return t(v); }' \
        'by reference with a copy the caller makes'
refuses 'int t(__int128); int g(__int128 v){ return t(v); }' \
        'callee-saved there'
echo "a struct that must travel by reference, and the types lowered
through the registers Windows makes callee-saved, are refused by name"
