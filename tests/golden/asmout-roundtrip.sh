#!/bin/sh
# `-S` must emit the OBJECT, as text -- on every target.
#
# EmbCC's -S is not a codegen stage the way GCC's is. The backend has
# already chosen encodings, so -S prints those BYTES as .byte directives
# with the relocations attached explicitly, and an assembler is given
# nothing to choose. That buys a property GCC and Clang cannot offer:
# assembling the -S output reproduces the -c object exactly. This test
# is that property, and it is the only thing that makes -S trustworthy.
#
# It exists because -S was quietly wrong for three targets. The guard
# refused aarch64 and named the reason -- "emitting text that is not the
# object would be worse than refusing" -- and then thumb, riscv32 and
# riscv64 were added after that line and fell straight through it. They
# got the x86-64 disassembler run over their bytes: wrong instruction
# LENGTHS, so wrong .byte grouping, and x86 mnemonics in the comments.
# `.byte 0x13,0x01  # adc (%rcx),%eax` for a RISC-V `addi`.
#
# Three things had to be true per target, and each is checked here by
# construction rather than by reading:
#   * instruction lengths (src/arch/target.c) -- or the bytes regroup
#   * relocation NAMES (asmout.c) -- or the assembler rejects it
#   * the RISC-V pcrel pair, whose low half names the auipc's ADDRESS
#     and not the symbol, spelled with a local label as an assembler does
set -u
echo "TEST-MARKER asmout-roundtrip"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
out=$EMBCC_ROOT/tests/golden/out/asmout-roundtrip
rm -rf "$out"; mkdir -p "$out"

command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "skipped: llvm-mc / llvm-objcopy not found"; exit 0; }

# Chosen to force every relocation kind these targets emit: a call to an
# undefined symbol, a string literal's address, a global's address, a
# pointer slot in .data, and .bss.
cat > "$out/u.c" <<'CEOF'
extern int helper(int);
static const char msg[] = "hello, world";
int table[4] = { 1, 2, 3, 4 };
int scratch[8];
const char *greet(void) { return msg; }
int *where(void) { return table; }
int compute(int x) { return helper(x) + table[1] + scratch[2]; }
long mix(long a, long b) { return a * b - (a >> 3) + (b & 0xff); }
CEOF

fail=0
for spec in "x86_64-elf:x86_64:" \
            "aarch64-elf:aarch64:" \
            "thumbv7m-none-eabi:thumbv7m:" \
            "riscv32-unknown-elf:riscv32:-mattr=+m" \
            "riscv64-unknown-elf:riscv64:-mattr=+m"; do
    t=${spec%%:*}; rest=${spec#*:}; mc=${rest%%:*}; attr=${rest#*:}
    d="$out/$t"; mkdir -p "$d"

    "$EMBCC" --target="$t" -S "$out/u.c" -o "$d/u.s" 2> "$d/s.err" || {
        echo "FAIL $t: -S refused"; sed 's/^/     | /' "$d/s.err"; fail=1
        continue; }
    "$EMBCC" --target="$t" -c "$out/u.c" -o "$d/direct.o" 2>/dev/null || {
        echo "FAIL $t: -c failed on the same unit"; fail=1; continue; }

    # shellcheck disable=SC2086
    "$MC" -triple="$mc" $attr -filetype=obj "$d/u.s" -o "$d/reasm.o" \
        2> "$d/mc.err" || {
        echo "FAIL $t: llvm-mc rejected the -S output"
        head -4 "$d/mc.err" | sed 's/^/     | /'; fail=1; continue; }

    "$OBJCOPY" -O binary --only-section=.text "$d/direct.o" "$d/direct.bin" \
        2>/dev/null
    "$OBJCOPY" -O binary --only-section=.text "$d/reasm.o"  "$d/reasm.bin" \
        2>/dev/null
    if cmp -s "$d/direct.bin" "$d/reasm.bin"; then
        echo "  $t: .text reassembles byte-identically ($(wc -c < "$d/direct.bin" | tr -d ' ') bytes)"
    else
        echo "FAIL $t: the reassembled .text differs from the object"
        od -An -tx1 "$d/direct.bin" > "$d/a.txt"
        od -An -tx1 "$d/reasm.bin"  > "$d/b.txt"
        diff "$d/a.txt" "$d/b.txt" | head -8 | sed 's/^/     | /'
        fail=1
    fi
done

# And the grouping itself, which is what silently broke: a fixed-width
# target must not produce a .byte line of the wrong width. RISC-V
# without the C extension is 4 bytes per instruction, always.
# `grep -c` prints 0 AND exits 1 when nothing matches, so `|| echo 0`
# would append a second zero and the comparison below would see "0\n0".
rv=$out/riscv64-unknown-elf/u.s
n=$(grep -c '^	\.byte	0x..,0x..,0x..,0x..$' "$rv" 2>/dev/null) || n=0
bad=$(grep -c '^	\.byte	0x..$' "$rv" 2>/dev/null) || bad=0
if [ "$n" -gt 0 ] && [ "$bad" -eq 0 ]; then
    echo "  riscv64: every instruction line is 4 bytes wide ($n of them)"
else
    echo "FAIL riscv64: instruction grouping is wrong ($n four-byte, $bad single-byte)"
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
