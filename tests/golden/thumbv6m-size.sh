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

echo "thumbv6m-size: va_arg is a word load; a slot's value is read from the register that holds it; a constant is made where it is read"
