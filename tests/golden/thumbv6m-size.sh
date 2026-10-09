#!/bin/sh
# ARMv6-M code size: the shapes that cost bytes, pinned (the Cortex-M0
# counterpart of thumb-size.sh). Each check compiles a small function for
# thumbv6m-none-eabi at -Os and reads its disassembly:
#   - va_arg reads its word with one LDR: the argument area is aligned,
#     and an access marked unaligned is a byte at a time on ARMv6-M
#     (thirteen instructions a word).
#   - the slot cache (v6m.c): a value in a frame slot, stored and read
#     again with nothing in between writing the register or the slot, is
#     read from the register. Checked on functions whose values cross
#     calls at a pool of two registers (EMBCC_RA_MAXPOOL), against the
#     same build with EMBCC_V6_NOSLOTCACHE=1, and on strtol's conv, whose
#     values crossing calls outnumber the callee-saved registers.
#   - rematerialization: a constant with no register is made where it is
#     read (a MOVS, or its pool word), never stored to a slot.
set -u
echo "TEST-MARKER thumbv6m-size"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP: no $OBJDUMP"; exit 0; }
T=thumbv6m-none-eabi
out=tests/golden/out/thumbv6m-size
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
dis() { "$OBJDUMP" -d --no-show-raw-insn --triple=thumbv6m "$1" |
            sed -n "/<$2>:/,/^\$/p"; }
cc6() { "$EMBCC" --target=$T -Os -c "$1" -o "$2" || fail "compile $1"; }

# ---- va_arg ---------------------------------------------------------------
cat > "$out/va.c" <<'EOF'
int vsum(int n, ...)
{
    __builtin_va_list ap;
    int s = 0;
    __builtin_va_start(ap, n);
    while (n--)
        s += __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    return s;
}
long long vsum64(int n, ...)
{
    __builtin_va_list ap;
    long long s = 0;
    __builtin_va_start(ap, n);
    while (n--)
        s += __builtin_va_arg(ap, long long);
    __builtin_va_end(ap);
    return s;
}
double vlast(int n, ...)
{
    __builtin_va_list ap;
    double d = 0;
    __builtin_va_start(ap, n);
    while (n--)
        d = __builtin_va_arg(ap, double);
    __builtin_va_end(ap);
    return d;
}
EOF
cc6 "$out/va.c" "$out/va.o"
for f in vsum vsum64 vlast; do
    dis "$out/va.o" $f > "$out/$f.dis"
    grep -q 'ldr[[:space:]]' "$out/$f.dis" || { cat "$out/$f.dis"; fail "$f: no word load"; }
    if grep -q 'ldrb' "$out/$f.dis"; then
        cat "$out/$f.dis"; fail "$f: va_arg reads its argument a byte at a time"
    fi
done

# ---- the slot cache --------------------------------------------------------
cat > "$out/sc.c" <<'EOF'
extern int g(int);
int chain(int a, int b, int c, int d)
{
    int x = g(a) + b;
    int y = g(x ^ c);
    return y + x + d;
}
int chain2(int a, int b, int c)
{
    int x = g(a) + b;
    int y = g(x * c);
    return g(y + c) + x;
}
int chain3(int *p, int b)
{
    int x = g(*p) + b;
    p[1] = x;
    p[2] = g(x);
    return x;
}
EOF
ninsn() { dis "$1" $2 | grep '^ *[0-9a-f]*:' | grep -vc 'nop'; }
EMBCC_RA_MAXPOOL=2 "$EMBCC" --target=$T -Os -c "$out/sc.c" -o "$out/sc.o" ||
    fail "compile sc.c"
EMBCC_V6_NOSLOTCACHE=1 EMBCC_RA_MAXPOOL=2 "$EMBCC" --target=$T -Os \
    -c "$out/sc.c" -o "$out/sc0.o" || fail "compile sc.c without the cache"
for f in chain chain2 chain3; do
    on=$(ninsn "$out/sc.o" $f); off=$(ninsn "$out/sc0.o" $f)
    [ "$on" -lt "$off" ] || { dis "$out/sc.o" $f
        fail "$f: $on instructions with the slot cache, $off without"; }
done
# At a label every jump to which comes before it, what holds is what holds
# on every way in: x is stored from r1 at the end of both arms, and the
# add after the join reads r1.
cat > "$out/pick.c" <<'EOF'
extern int g(int);
int pick(int a, int b, int c)
{
    int x;
    if (c)
        x = g(a) + 1;
    else
        x = g(b) + 2;
    return g(x + 7) + x;
}
EOF
EMBCC_RA_MAXPOOL=2 "$EMBCC" --target=$T -Os -c "$out/pick.c" -o "$out/pick.o" ||
    fail "compile pick.c"
dis "$out/pick.o" pick > "$out/pick.dis"
nsp=$(grep -cE 'ldr[[:space:]].*\[sp' "$out/pick.dis") || nsp=0
[ "$nsp" -le 2 ] || { cat "$out/pick.dis"
    fail "pick: x is loaded at the join though both arms leave it in a register"; }
# A load goes into the scratch role that holds nothing the instruction
# reads: x * c after the join, with x in r6 and the fixed roles (r6, r7),
# loads c into r7 and keeps x.
sed 's/g(x + 7)/g(x * c)/; s/pick(/pick2(/' "$out/pick.c" > "$out/pick2.c"
EMBCC_T_EXT=0 EMBCC_RA_MAXPOOL=2 "$EMBCC" --target=$T -Os -c "$out/pick2.c" \
    -o "$out/pick2.o" || fail "compile pick2.c"
dis "$out/pick2.o" pick2 > "$out/pick2.dis"
nsp=$(grep -cE 'ldr[[:space:]].*\[sp' "$out/pick2.dis") || nsp=0
[ "$nsp" -le 1 ] || { cat "$out/pick2.dis"
    fail "pick2: loading c into the role that holds x loads x again"; }
# A spill store none of whose reads loads it is left out: chain's x is
# read from r6 on both sides of the call (r6 survives it), so nothing
# stores x -- every stack store's slot is loaded somewhere.
dis "$out/sc.o" chain > "$out/chain.dis"
dead=$(awk '/\tstr\t.*\[sp/ { s[$NF] = 1 } /\tldr\t.*\[sp/ { l[$NF] = 1 }
            END { n = 0; for (k in s) if (!(k in l)) n++; print n }' \
           "$out/chain.dis")
[ "$dead" = 0 ] || { cat "$out/chain.dis"
    fail "chain: $dead stack slots stored and never loaded"; }
# The high word of a 64-bit value in a slot, read alone ((int)(x >> 32)),
# is loaded into the register the result goes to, not a scratch and a
# copy (tests/exec/spill-hiword.c's hw, whose values the boards check).
"$EMBCC" --target=$T -Os -c tests/exec/spill-hiword.c -o "$out/hw.o" ||
    fail "compile spill-hiword.c"
dis "$out/hw.o" hw > "$out/hw.dis"
if awk '/\tldr\t.*\[sp/ { r = $3; sub(",", "", r); next }
        /\tmov\t/ && r != "" { s = $4; if (s == r) bad = 1 } { r = "" }
        END { exit !bad }' "$out/hw.dis"; then
    cat "$out/hw.dis"; fail "hw: a high word loaded into a scratch and copied"
fi
"$EMBCC" --target=$T -Os -Ilib/libc/include -c lib/libc/src/stdlib/strtol.c \
    -o "$out/strtol.o" || fail "compile strtol.c"
dis "$out/strtol.o" conv > "$out/conv.dis"
nsp=$(grep -cE '(ldr|str)[a-z]*[[:space:]].*\[sp' "$out/conv.dis") || nsp=0
[ "$nsp" -le 70 ] ||
    fail "strtol's conv makes $nsp stack accesses (80 before the slot cache)"

# ---- rematerialization ----------------------------------------------------
# A constant with no register is made where it is read, never stored to a
# slot: at a pool of r0-r3 the 1000 every call passes (one value, after
# value numbering) has none, and with EMBCC_T_NOREMAT=1 it is built once,
# stored, and loaded at each call.
cat > "$out/rm.c" <<'EOF'
extern void w(const char *, int, int);
void three(const char *s, const char *t, int a, int b)
{
    w(s, 1000, a);
    w(t, 1000, b);
    w(s, 1000, a + b);
    w(t, 1000, a - b);
}
EOF
EMBCC_RA_MAXPOOL=4 "$EMBCC" --target=$T -Os -c "$out/rm.c" -o "$out/rm.o" ||
    fail "compile rm.c"
EMBCC_T_NOREMAT=1 EMBCC_RA_MAXPOOL=4 "$EMBCC" --target=$T -Os \
    -c "$out/rm.c" -o "$out/rm0.o" || fail "compile rm.c without remat"
dis "$out/rm.o" three > "$out/three.dis"
on=$(ninsn "$out/rm.o" three); off=$(ninsn "$out/rm0.o" three)
[ "$on" -lt "$off" ] || { cat "$out/three.dis"
    fail "three: $on instructions made again where read, $off stored"; }
[ "$(grep -c 'ldr[[:space:]]*r1, \[pc' "$out/three.dis")" -eq 4 ] ||
    { cat "$out/three.dis"; fail "three: 1000 is not made at each of its four reads"; }

# ---- a comparison as a value ------------------------------------------------
# == and != against zero or anything, < and >= 0, the unsigned orders and
# a float's == and != (the helper's answer against zero) are made without
# a branch: `negs; adcs`, `subs; sbcs`, the sign, the carry. The values
# themselves are tests/exec/cmp-value.c's, on the board.
cat > "$out/cc.c" <<'EOF'
int eq0(int a) { return a == 0; }
int ne0(int a) { return a != 0; }
int eqk(int a) { return a == 45; }
int ner(int a, int b) { return a != b; }
int neg(int a) { return a < 0; }
int nneg(int a) { return a >= 0; }
int ltu(unsigned a, unsigned b) { return a < b; }
int geu(unsigned a) { return a >= 10; }
int gtu(unsigned a, unsigned b) { return a > b; }
int leu(unsigned a) { return a <= 2; }
int feq(float x, float y) { return x == y; }
int dne(double x, double y) { return x != y; }
EOF
cc6 "$out/cc.c" "$out/cc.o"
for f in eq0 ne0 eqk ner neg nneg ltu geu gtu leu feq dne; do
    dis "$out/cc.o" $f > "$out/$f.dis"
    if grep -Eq '[[:space:]]b(eq|ne|cs|hs|cc|lo|mi|pl|hi|ls|ge|lt|gt|le)[[:space:]]' "$out/$f.dis"; then
        cat "$out/$f.dis"; fail "$f: the comparison's value is made with a branch"
    fi
done

# ...and so is a symbol's address: its pool word at each call, never a
# stack slot.
cat > "$out/ra.c" <<'EOF'
extern void w(const char *, int, int);
extern int tab[];
void addrs(int a, int b)
{
    w((const char *)tab, a, 1);
    w((const char *)tab, b, 2);
    w((const char *)tab, a + b, 3);
    w((const char *)tab, a - b, 4);
}
EOF
EMBCC_RA_MAXPOOL=4 "$EMBCC" --target=$T -Os -c "$out/ra.c" -o "$out/ra.o" ||
    fail "compile ra.c"
dis "$out/ra.o" addrs > "$out/addrs.dis"
[ "$(grep -c 'ldr[[:space:]]*r0, \[pc' "$out/addrs.dis")" -eq 4 ] ||
    { cat "$out/addrs.dis"; fail "addrs: tab's address is not made at each of its four reads"; }

# ---- 64-bit tests ---------------------------------------------------------
# A 64-bit operand loaded into temporaries of the instruction's own is
# tested in them: `orrs lo, hi`, `cmp lo, blo; sbcs hi, bhi`, the halves
# EORed in place -- not copied into a third register first (which, with
# everything in memory, cost a push and a pop as well).
cat > "$out/w64.c" <<'EOF'
int lt64(const long long *p) { return p[0] < p[1]; }
int eq64(const long long *p) { return p[0] == p[1]; }
int nz64(const long long *p) { if (p[0]) return 3; return 5; }
long long sel64(const long long *p) { return p[0] ? p[1] : p[2]; }
EOF
EMBCC_RA_MAXPOOL=0 "$EMBCC" --target=$T -Os -c "$out/w64.c" -o "$out/w64.o" ||
    fail "compile w64.c"
for f in lt64 eq64 nz64 sel64; do
    dis "$out/w64.o" $f > "$out/$f.dis"
    if awk '/\tmov\t/ { m = 1; next } /\t(sbcs|orrs|eors)\t/ && m { bad = 1 } { m = 0 }
            END { exit !bad }' "$out/$f.dis"; then
        cat "$out/$f.dis"; fail "$f: a 64-bit operand copied before its test"
    fi
done

# ---- a jump to the next instruction ----------------------------------------
# p(c) ? c + 32 : c -- the else arm is no code (c is already in its
# register), so the then arm's jump over it lands on the next
# instruction: none is made.
cat > "$out/nj.c" <<'EOF'
extern int p(int);
int tl(int c) { return p(c) ? c + 32 : c; }
EOF
cc6 "$out/nj.c" "$out/nj.o"
dis "$out/nj.o" tl > "$out/tl.dis"
# (a B's offset is from its address + 4: to the next one is -2)
if grep -Eq '[[:space:]]b[[:space:]].*imm = #-0x2$' "$out/tl.dis"; then
    cat "$out/tl.dis"; fail "tl: a jump to the instruction after it"
fi

# ---- trampolines ----------------------------------------------------------
# Six compares branch to one label 600 bytes on: one `b<!c> 1f; b err`
# pair, and the other five a single b<c> to that pair's jump.
{
    echo "extern void big(int);"
    echo "int far(int c, int x)"
    echo "{"
    for k in 1 3 5 7 9 11; do echo "    if (c == $k) goto err;"; done
    k=0
    while [ $k -lt 80 ]; do echo "    big(x + $k);"; k=$((k + 1)); done
    echo "    return x;"
    echo "err:"
    echo "    return -1;"
    echo "}"
} > "$out/far.c"
cc6 "$out/far.c" "$out/far.o"
dis "$out/far.o" far > "$out/far.dis"
pairs=$(awk '/\tb(eq|ne)\t/ { c = 1; next } /\tb\t/ && c { n++ } { c = 0 }
             END { print n + 0 }' "$out/far.dis")
[ "$pairs" -le 1 ] || { cat "$out/far.dis"
    fail "far: $pairs conditional branches over a jump, where one does"; }

echo "thumbv6m-size: va_arg is a word load; a slot's value is read from the register that holds it; a constant is made where it is read; a comparison's value has no branch"
