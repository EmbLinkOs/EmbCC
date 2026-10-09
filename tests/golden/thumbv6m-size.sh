#!/bin/sh
# ARMv6-M code size: the shapes that cost bytes, pinned (the Cortex-M0
# counterpart of thumb-size.sh). Each check compiles a small function for
# thumbv6m-none-eabi at -Os and reads its disassembly:
#   - va_arg reads its word with one LDR: the argument area is aligned,
#     and an access marked unaligned is a byte at a time on ARMv6-M
#     (thirteen instructions a word).
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

echo "thumbv6m-size: va_arg is a word load"
