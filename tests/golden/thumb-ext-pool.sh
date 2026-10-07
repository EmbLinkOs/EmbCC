#!/bin/sh
# r9, r10 and r11 as allocatable registers on Thumb.
#
# The ARMv7-M lowering keeps three registers for itself -- an address,
# a temporary and a scratch -- and they were always r9-r11, so the
# allocator had five callee-saved registers where clang has eight: a loop
# with ten values live across calls kept five of them on the stack. Each
# function is now also generated with r9-r11 in the pool and the three
# roles taken, instruction by instruction, from whichever of r9-r11 is
# free there (an attempt where one is not is dropped), and the shortest
# attempt is kept. EMBCC_T_EXT=0 turns the extended attempts off, and
# EMBCC_T_EXT=1 keeps one whenever it succeeds -- which is how this test
# runs the exec programs, so the roles' choice is proved on code whose
# shortest form would not have used them.
set -u
echo "TEST-MARKER thumb-ext-pool"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/thumb-ext-pool
rm -rf "$out"; mkdir -p "$out"

cat > "$out/m.c" <<'EOT'
extern int g(int);
int many(int a, int b, int c, int d, int n)
{
    int e = a * 3, f = b * 5, h = c * 7, i = d * 9, j = a + b, k = c + d;
    int l = a ^ d, m = b ^ c, o = a - c, p = b - d;
    for (int x = 0; x < n; x++) {
        e += g(x); f ^= g(e); h += f; i -= h; j += i; k ^= j; l += k;
        m -= l; o += m; p ^= o;
    }
    return e + f + h + i + j + k + l + m + o + p;
}
EOT
size_of() {                     # size_of OBJ -> bytes of `many`
    "$OD" -t "$1" | awk '/ many$/ { print $5 }' | sed 's/^0*//' |
        { read h; echo $((0x${h:-0})); }
}
EMBCC_T_EXT=0 "$EMBCC" --target=thumbv7em-none-eabi -O2 -c "$out/m.c" -o "$out/m0.o" &&
"$EMBCC" --target=thumbv7em-none-eabi -O2 -c "$out/m.c" -o "$out/m.o" || {
    echo "FAIL: could not compile"; exit 1; }
s0=$(size_of "$out/m0.o"); s=$(size_of "$out/m.o")
"$OD" -d --no-show-raw-insn --triple=thumbv7em "$out/m.o" > "$out/m.dis"
uses=$(grep -vE 'push|pop' "$out/m.dis" | grep -cE '\b(r9|r10|r11)\b' || true)
[ "$s" -lt "$s0" ] && [ "$uses" -ge 10 ] || {
    echo "FAIL: ten values live across calls should use r9-r11 (got $s bytes,"
    echo "$s0 without them, $uses instructions naming one):"; cat "$out/m.dis"; exit 1; }
echo "ten values across calls: $s bytes with r9-r11 allocatable, $s0 without"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP (exec): $QEMU not found (set EMBCC_QEMU_ARM)"; exit 0; }
command -v "$CLANG" >/dev/null 2>&1 || {
    echo "SKIP (exec): no reference compiler for thumbv7m"; exit 0; }
T=thumbv7m-none-eabi
H=tests/harness/thumb
export EMBCC_THUMB_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
        echo "FAIL: the harness does not compile for $T"; exit 1; }
done
"$EMBCC" --target=$T -Os -c lib/rt/int64.c -o "$out/int64.o" &&
"$EMBCC" --target=$T -Os -c lib/rt/softfp.c -o "$out/softfp.o" &&
"$EMBCC" --target=$T -Os -Ilib/libc/include -c lib/libc/src/math/sqrt.c \
         -o "$out/sqrt.o" || { echo "FAIL: the runtime does not compile"; exit 1; }

run() {                         # run TAG OBJ... -> $out/TAG.txt
    tag=$1; shift
    sh "$H/link.sh" "$out/$tag.elf" "$@" || {
        echo "FAIL: $tag: embld could not link the image"; exit 1; }
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" --until '==END==' \
        "$QEMU" -M lm3s6965evb -cpu cortex-m3 -nographic \
        -kernel "$out/$tag.elf" > "$out/$tag.txt" 2>/dev/null
    grep -q '==END==' "$out/$tag.txt" || {
        echo "FAIL: $tag: the image did not reach the end of main:"
        sed -n '1,10p' "$out/$tag.txt"; exit 1; }
}
agree() {                       # agree REF TAG
    diff -u "$out/$1.txt" "$out/$2.txt" > "$out/$2.diff" || {
        echo "FAIL: $2 does not agree with the reference:"
        head -20 "$out/$2.diff"; exit 1; }
}

# embedded-stress against clang; the 64-bit, comparison and float
# programs against the host.
"$CLANG" -target $T -ffreestanding -Os -c tests/golden/embedded-stress.c \
         -o "$out/stress-ref.o" || { echo "FAIL: clang could not compile"; exit 1; }
run stress-ref "$out/stress-ref.o"
for p in int64 cmp64 float; do
    cc -std=c99 -w -o "$out/host-$p" tests/golden/embedded-$p.c "$H/hostio.c" -lm || {
        echo "FAIL: embedded-$p.c does not compile for the host"; exit 1; }
    "$out/host-$p" > "$out/$p-ref.txt" || { echo "FAIL: host $p failed"; exit 1; }
done
for opt in -O1 -O2 -Os; do
    for p in stress int64 cmp64 float; do
        EMBCC_T_EXT=1 "$EMBCC" --target=$T $opt -c tests/golden/embedded-$p.c \
                 -o "$out/$p$opt.o" || {
            echo "FAIL: embedded-$p.c at $opt does not compile"; exit 1; }
    done
    run stress$opt "$out/stress$opt.o"; agree stress-ref stress$opt
    run int64$opt "$out/int64$opt.o" "$out/int64.o"; agree int64-ref int64$opt
    run cmp64$opt "$out/cmp64$opt.o"; agree cmp64-ref cmp64$opt
    run float$opt "$out/float$opt.o" "$out/softfp.o" "$out/int64.o" "$out/sqrt.o"
    agree float-ref float$opt
done
echo "stress, int64, cmp64, float with r9-r11 allocatable: agree at -O1, -O2, -Os"
