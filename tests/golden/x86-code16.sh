#!/bin/sh
# x86's 16- and 32-bit code in the AT&T assembler (.code16, .code32,
# .code64 in one file: a boot sector, a protected-mode stub, long mode),
# against llvm-mc: the same bytes and the same relocations (R_X86_64_16
# where 16-bit code names a label). The forms the 8086 has that 64-bit
# code lost, 16-bit addressing, the operand- and address-size prefixes,
# rel16 branches, far jumps, moffs loads; and what is only in 64-bit code
# refused by name in 16-bit code.
set -u
echo "TEST-MARKER x86-code16"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
here=$PWD/tests/golden/x86-code16
out=$PWD/tests/golden/out/x86-code16
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
MC=$(command -v llvm-mc || true)
[ -n "$MC" ] && command -v llvm-objcopy >/dev/null && command -v llvm-readelf >/dev/null ||
    { echo "SKIP: no llvm-mc / llvm-objcopy / llvm-readelf"; exit 0; }

"$EMBCC" --target=x86_64-elf -c "$here/modes.s" -o "$out/m.o" 2> "$out/m.err" ||
    fail "EmbCC refused modes.s: $(head -5 "$out/m.err")"
"$MC" -triple=x86_64 -filetype=obj "$here/modes.s" -o "$out/m.ref.o" ||
    fail "llvm-mc refused modes.s"
for f in m m.ref; do
    llvm-objcopy -O binary -j .text "$out/$f.o" "$out/$f.bin"
    llvm-readelf -r "$out/$f.o" | grep R_X86 | awk '{print $1, $3}' > "$out/$f.rel"
done
cmp -s "$out/m.bin" "$out/m.ref.bin" || {
    cmp -l "$out/m.bin" "$out/m.ref.bin" | head -5
    fail "modes.s: the bytes differ from llvm-mc's"; }
diff "$out/m.ref.rel" "$out/m.rel" > "$out/rel.diff" ||
    fail "modes.s: the relocations differ: $(cat "$out/rel.diff")"
[ "$(grep -c R_X86_64_16 "$out/m.rel")" -ge 4 ] || fail "no R_X86_64_16 read"
echo "$(wc -c < "$out/m.bin" | tr -d ' ') bytes of 16-, 32- and 64-bit code as llvm-mc writes them"

# lock and rep among the other prefixes, in llvm-mc's order; a branch to
# an address (*ABS*); alignment fills in code and data
for f in prefixes absjump align riprel intel; do
    # LLVM 18 pads `.balign 8, 0x90` in code with long nops instead of the
    # fill asked for; EmbCC writes the fill, as LLVM 23 does
    if [ "$f" = align ] && w=$(llvm_too_old "$MC"); then
        echo "align.s skipped: $w"; continue
    fi
    "$EMBCC" --target=x86_64-elf -c "$here/$f.s" -o "$out/$f.o" 2> "$out/$f.err" ||
        fail "EmbCC refused $f.s: $(head -3 "$out/$f.err")"
    "$MC" -triple=x86_64 -mattr=+avx512f -filetype=obj "$here/$f.s" -o "$out/$f.ref.o" || fail "llvm-mc refused $f.s"
    for o in $f $f.ref; do
        llvm-objdump -s -j .text -j .data "$out/$o.o" | grep -v 'file format' > "$out/$o.dump"
        llvm-readelf -r "$out/$o.o" | grep R_X86 | awk '{print $1, $3, $NF}' > "$out/$o.rel"
    done
    cmp -s "$out/$f.dump" "$out/$f.ref.dump" ||
        fail "$f.s: the bytes differ from llvm-mc's: $(diff "$out/$f.ref.dump" "$out/$f.dump" | head -6)"
    cmp -s "$out/$f.rel" "$out/$f.ref.rel" ||
        fail "$f.s: the relocations differ: $(diff "$out/$f.ref.rel" "$out/$f.rel" | head -6)"
done
echo "lock/rep placement, branches to an address, alignment fills, rip-relative labels, .intel_syntax: llvm-mc's bytes"

refs() {
    printf '\t.code16\n\t%s\n' "$2" > "$out/bad.s"
    if "$EMBCC" --target=x86_64-elf -c "$out/bad.s" -o "$out/bad.o" 2> "$out/bad.err"; then
        fail "$1 was accepted in 16-bit code"
    fi
    grep -q -- "$3" "$out/bad.err" || fail "$1 was refused, but not by name: $(cat "$out/bad.err")"
}
refs "a 64-bit register" 'movq %rax, %rbx' "only in 64-bit code"
refs "r8" 'movw %r8w, %ax' "only in 64-bit code"
refs "%rip" 'movw 4(%rip), %ax' "only in 64-bit code"
refs "a scaled 16-bit index" 'movw (%bx,%si,2), %ax' "no scale"
refs "%ax as a base" 'movw (%ax), %cx' "not a base register"
printf '\t.code64\n\tpusha\n' > "$out/bad.s"
"$EMBCC" --target=x86_64-elf -c "$out/bad.s" -o "$out/bad.o" 2> "$out/bad.err" &&
    fail "pusha was accepted in 64-bit code"
grep -q "not in 64-bit code" "$out/bad.err" || fail "pusha refused, but not by name"
echo "six misuses refused by name"
