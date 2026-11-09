#!/bin/sh
# EmbCC's compiler runtime on macOS.
#
# EmbCC's code calls routines libSystem does not export: __divdc3 and
# __divsc3 for a complex division, __ashlti3, __lshrti3 and __ashrti3 for
# a 128-bit shift by a variable count, __multi3. clang takes them from its
# own libclang_rt.osx.a; EmbCC linked libSystem alone, and such a program
# did not link ("Undefined symbols: ___divdc3"). lib/rt is now built for
# Darwin (make rt-darwin-aarch64, rt-darwin-x86_64) and linked after
# -lSystem, so what the system has still comes from the system.
#
#  1. embar archives Mach-O members in the BSD format ld64 reads -- it
#     refuses the GNU one ("archive member '//' not a mach-o file") -- with
#     a __.SYMDEF table, and still writes the GNU one for ELF. Laid out as
#     Apple's ar lays it out: every member header, which is what the
#     table's offsets name, on a multiple of eight. Xcode 16's ld refused
#     the runtime of the 0.1.0 dry run, whose members had odd sizes
#     ("archive member invalid control bits"); a newer ld accepted it, so
#     linking on this Mac proves nothing. On a Mac the archive must be
#     byte for byte what `ZERO_AR_DATE=1 ar rcs` writes.
#  2. On an arm64 Mac: complex division, 128-bit shifts, multiply and
#     compares link and print what clang's build prints, at -O0 and -O2.
#  3. An object's LC_BUILD_VERSION says the macOS the link says: 10.13 for
#     x86-64 (it said 11.0, and ld64 warned on every link), or
#     -mmacosx-version-min's.
set -u
echo "TEST-MARKER darwin-rt"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBAR=${EMBAR:-./embar}
out=$PWD/tests/golden/out/darwin-rt
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- 1. the archive format -------------------------------------------------
printf 'int a_long_member_name_function(void) { return 1; }\nint other;\n' > "$out/m.c"
"$EMBCC" --target=aarch64-apple-darwin -c "$out/m.c" -o "$out/a_long_member_name.o" ||
    fail "darwin: does not compile"
"$EMBCC" --target=riscv32-unknown-elf -c "$out/m.c" -o "$out/a_long_member_name_elf.o" ||
    fail "riscv32: does not compile"
rm -f "$out/macho.a" "$out/elf.a"
"$EMBAR" rcs "$out/macho.a" "$out/a_long_member_name.o" || fail "embar (Mach-O)"
"$EMBAR" rcs "$out/elf.a" "$out/a_long_member_name_elf.o" || fail "embar (ELF)"
head -c 72 "$out/macho.a" | tail -c 64 | grep -q '^#1/' &&
    grep -q '__\.SYMDEF' "$out/macho.a" || fail "a Mach-O archive is not BSD-format with __.SYMDEF"
llvm-nm --print-armap "$out/macho.a" 2>/dev/null | grep -q '_a_long_member_name_function in a_long_member_name.o' ||
    { llvm-nm --print-armap "$out/macho.a"; fail "the Mach-O archive's index lacks the function"; }
[ "$("$EMBAR" t "$out/macho.a")" = a_long_member_name.o ] ||
    fail "embar does not read its BSD archive back"
[ "$(head -c 10 "$out/elf.a" | tail -c 2)" = "/ " ] &&
    ! grep -q '__\.SYMDEF' "$out/elf.a" ||
    fail "an ELF archive is no longer GNU-format (a / index first)"
echo "darwin-rt: embar writes Mach-O archives in the BSD format with __.SYMDEF, ELF ones in the GNU one"

# three members of odd sizes, with names of 3, 10 and 17 characters
names="abc.o aeabidiv.o aeabi_divmod_ab.o"
n=0
for m in $names; do
    n=$((n + 1))
    printf 'int f%d(int x) { return x * %d; }\n' $n $n > "$out/l.c"
    k=0
    while [ $k -lt $((n * 3)) ]; do
        printf 'int g%d_%d(int y) { return y ^ %d; }\n' $n $k $k >> "$out/l.c"
        k=$((k + 1))
    done
    "$EMBCC" --target=aarch64-apple-darwin -c "$out/l.c" -o "$out/$m" || fail "darwin: $m"
done
rm -f "$out/layout.a"
embar_abs=$(cd "$(dirname "$EMBAR")" && pwd)/$(basename "$EMBAR")
(cd "$out" && "$embar_abs" rcs layout.a $names) || fail "embar (layout)"
python3 - "$out/layout.a" << 'EOF' || fail "a Mach-O archive's headers are not on multiples of eight"
import struct, sys
d = open(sys.argv[1], 'rb').read()
off, heads = 8, []
while off < len(d):
    h = d[off:off + 60]
    heads.append(off)
    off += 60 + int(h[48:58])
    off += off & 1          # an odd-sized member is followed by a newline
assert off == len(d), 'members overrun the file'
odd = [o for o in heads if o % 8]
# the table: its size, then (string, header offset) pairs
base = heads[0] + 60 + int(d[heads[0] + 3:heads[0] + 16])
n = struct.unpack('<I', d[base:base + 4])[0] // 8
targets = {struct.unpack('<I', d[base + 8 + 8 * i:base + 12 + 8 * i])[0] for i in range(n)}
bad = sorted(t for t in targets if t % 8 or t not in heads)
if odd or bad or n < 3:
    print('headers off eight:', odd, 'table offsets wrong:', bad, 'entries:', n)
    sys.exit(1)
EOF
echo "darwin-rt: every member header, and every offset the __.SYMDEF table names, is on a multiple of eight"
if [ "$(uname -s)" = Darwin ] && [ -x /usr/bin/ar ]; then
    rm -f "$out/apple.a"
    (cd "$out" && ZERO_AR_DATE=1 /usr/bin/ar rcs apple.a $names) 2> /dev/null &&
        cmp -s "$out/layout.a" "$out/apple.a" ||
        fail "embar's archive is not byte for byte Apple ar's (cmp $out/layout.a $out/apple.a)"
    echo "darwin-rt: the archive is byte for byte what Apple's ar writes"
fi

# ---- 3. the minimum macOS (any host: otool or llvm-objdump reads it) ------
"$EMBCC" --target=x86_64-apple-darwin -c "$out/m.c" -o "$out/x.o" || fail "x86-64 darwin"
"$EMBCC" --target=aarch64-apple-darwin -mmacosx-version-min=12.3 -c "$out/m.c" -o "$out/v.o" ||
    fail "arm64 darwin, -mmacosx-version-min"
llvm-objdump --macho --private-headers "$out/x.o" | grep -q 'minos 10.13' ||
    fail "an x86-64 object does not say macOS 10.13"
llvm-objdump --macho --private-headers "$out/v.o" | grep -q 'minos 12.3' ||
    fail "-mmacosx-version-min=12.3 is not the object's"
echo "darwin-rt: an object's minimum macOS is the link's: 10.13 for x86-64, or -mmacosx-version-min's"

# ---- 2. the runtime, live --------------------------------------------------
[ "$(uname -s)-$(uname -m)" = Darwin-arm64 ] && command -v clang > /dev/null 2>&1 || {
    echo "darwin-rt: the live part needs an arm64 Mac with clang (skipped)"; exit 0; }
cat > "$out/p.c" << 'EOF'
#include <complex.h>
#include <stdio.h>
static volatile int sh = 70, sh2 = 3;
int main(void)
{
    volatile double _Complex a = 1.0 + 2.0 * I, b = 3.0 - 1.0 * I;
    volatile float _Complex fa = 4.0f + 1.0f * I, fb = 1.0f - 2.0f * I;
    double _Complex q = a / b;
    float _Complex fq = fa / fb;
    unsigned __int128 x = ((unsigned __int128)0x0123456789abcdefull << 64) | 0xfedcba9876543210ull;
    __int128 y = -(__int128)x;
    unsigned __int128 p = x * (x >> sh);
    printf("%.6f %.6f %.6f %.6f\n", creal(q), cimag(q), crealf(fq), cimagf(fq));
    printf("%016llx %016llx\n", (unsigned long long)(x << sh >> 64), (unsigned long long)(x >> sh));
    printf("%016llx %016llx\n", (unsigned long long)((unsigned __int128)(y >> sh2) >> 64),
           (unsigned long long)(y >> sh2));
    printf("%016llx %016llx %d %d\n", (unsigned long long)(p >> 64), (unsigned long long)p,
           x > (unsigned __int128)y, y < (__int128)x);
    return 0;
}
EOF
clang -O1 -w "$out/p.c" -o "$out/p-clang" && "$out/p-clang" > "$out/want.txt" ||
    fail "clang's build"
for o in -O0 -O2; do
    "$EMBCC" --target=aarch64-apple-darwin $o "$out/p.c" -o "$out/p$o" 2> "$out/p$o.err" || {
        cat "$out/p$o.err"; fail "$o: does not link (make rt-darwin-aarch64?)"; }
    "$out/p$o" > "$out/got$o.txt" || fail "$o: does not run"
    cmp -s "$out/want.txt" "$out/got$o.txt" || {
        diff "$out/want.txt" "$out/got$o.txt"; fail "$o: not what clang's build prints"; }
done
"$EMBCC" --target=x86_64-apple-darwin "$out/p.c" -o "$out/px" 2> "$out/px.err" || {
    cat "$out/px.err"; fail "x86-64: does not link (make rt-darwin-x86_64?)"; }
! grep -q 'built for newer' "$out/px.err" || {
    cat "$out/px.err"; fail "x86-64: ld64 says an object is for a newer macOS than the link"; }
echo "darwin-rt: complex division and 128-bit shifts, multiply and compares link and print clang's answers (arm64 -O0/-O2; x86-64 links)"
