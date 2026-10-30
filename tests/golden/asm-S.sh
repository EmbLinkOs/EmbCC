#!/bin/sh
# `embcc -S` — the assembly the backend emitted (vision §4.3, §17).
#
# The acceptance is byte-identity, not plausibility: assemble the `-S`
# output with the GNU assembler and require the result to be the SAME
# OBJECT `embcc -c` produces. Anything less and the text is a story about
# the program rather than the program.
#
# That bar is what forced the design. A disassembly of the bytes, written
# as ordinary mnemonics, does NOT reassemble to the same object: an
# assembler chooses among encodings (`sub $0x10,%rsp` in four bytes where
# the backend wrote seven), resolves branches it can see (`0f 84` rel32
# becomes `74` rel8), and resolves references to symbols it can see
# (`lea add(%rip)` becomes a fixed displacement with no relocation at all).
# Measured on this corpus, that version miscompiled 10 of 89 programs
# SILENTLY. So the instructions are emitted as their bytes, with the
# disassembly as a comment, and each relocation is attached with an
# explicit `.reloc` — leaving the assembler nothing to choose.
set -eu
echo "TEST-MARKER asm-S"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/asm-S
rm -rf "$out"; mkdir -p "$out"

[ "${ARCH:-x86_64}" = x86_64 ] ||
    { echo "skipped: -S is x86-64 only (no aarch64 disassembler here)"; exit 0; }
AS=${EMBCC_REFEREE_AS:-x86_64-elf-as}
command -v "$AS" > /dev/null 2>&1 ||
    { echo "skipped: $AS absent"; exit 0; }

# ---- the shape of the output ----------------------------------------------
cat > "$out/p.c" << 'EOF'
extern int g;
extern int puts(const char *);
static int helper(int x) { return x * 3; }
int (*hook)(int) = helper;
const char *name = "embcc";
int f(int n) { puts(name); return helper(n) + g; }
EOF
"$EMBCC" -S "$out/p.c" -o "$out/p.s"
sed -n '1,12p' "$out/p.s"

grep -q "^	.text" "$out/p.s"        || { echo "FAIL: no .text"; exit 1; }
grep -q "^	.globl	f"  "$out/p.s"     || { echo "FAIL: no .globl"; exit 1; }
grep -q "^f:"           "$out/p.s"     || { echo "FAIL: no label"; exit 1; }
grep -q "^	.size	f, .-f" "$out/p.s" || { echo "FAIL: no .size"; exit 1; }
# Every instruction carries its disassembly, so the file is readable even
# though the bytes are what assemble.
grep -qE "^	\.byte	0x[0-9a-f]+.*# " "$out/p.s" ||
    { echo "FAIL: instructions are not annotated"; exit 1; }
# Relocations are attached, not spelled into the instruction.
grep -q "\.reloc\s.*R_X86_64_PLT32.*puts" "$out/p.s" ||
    { echo "FAIL: the call to puts has no relocation"; exit 1; }
grep -q "\.reloc\s.*R_X86_64_PC32.*g" "$out/p.s" ||
    { echo "FAIL: the reference to g has no relocation"; exit 1; }
# A function pointer in an initializer is an address the linker fills in.
grep -q "^	.quad	helper" "$out/p.s" ||
    { echo "FAIL: hook's initializer lost its relocation"; exit 1; }
echo "the form: sections, symbols, sizes, each instruction as bytes with its
disassembly, relocations attached with .reloc, and .quad for a pointer
initializer"

# ---- the acceptance: it reassembles to the same object --------------------
"$EMBCC" -c "$out/p.c" -o "$out/ref.o"
"$AS" "$out/p.s" -o "$out/asm.o"
readelf -x .text "$out/ref.o" | tail -n +3 > "$out/ref.txt"
readelf -x .text "$out/asm.o" | tail -n +3 > "$out/asm.txt"
cmp "$out/ref.txt" "$out/asm.txt" ||
    { echo "FAIL: reassembling -S gives different code"; exit 1; }
echo "reassembled with $AS: .text byte-identical"

# ---- a thread-local's offset is not PC-relative ---------------------------
# x86-64 measures a rel32 from the END of its instruction, so a PC-relative
# .reloc carries -4. TPOFF32 is an offset from the thread pointer and
# carries nothing; given the -4 too, every __thread access assembled to
# the variable four bytes below.
# The code's relocations, which are the ones a .reloc writes; data's are
# .quad lines, which an assembler is free to rebase on a section symbol.
relocs() {
    LC_ALL=C readelf -rW "$1" |
        LC_ALL=C awk '/^Relocation section/ { t = ($3 == "'"'"'.rela.text'"'"'") }
             t && /^0/ { $2 = ""; $4 = ""; print }' | LC_ALL=C sort
}
cat > "$out/tls.c" << 'EOF'
__thread int t;
__thread long u = 7;
int *at(void) { return &t; }
long get(void) { return u + t; }
EOF
"$EMBCC" -c "$out/tls.c" -o "$out/tls-ref.o"
"$EMBCC" -S "$out/tls.c" -o "$out/tls.s"
"$AS" "$out/tls.s" -o "$out/tls-asm.o"
relocs "$out/tls-ref.o" > "$out/tls-ref.txt"
relocs "$out/tls-asm.o" > "$out/tls-asm.txt"
grep -q TPOFF32 "$out/tls-ref.txt" ||
    { echo "FAIL: no TPOFF32 in the object"; exit 1; }
diff "$out/tls-ref.txt" "$out/tls-asm.txt" ||
    { echo "FAIL: -S spells a thread-local's relocation differently"; exit 1; }
echo "__thread: -S then $AS gives the same TPOFF32 relocations as -c"

# ---- and over the whole execution corpus ----------------------------------
# Every program the suite runs, compiled both ways, compared byte for byte.
ident=0; bad=0
for f in tests/exec/*.c; do
    case "$(sed -n 's|.*// target: *\([a-z0-9_-]*\).*|\1|p' "$f" | head -1)" in
        aarch64*) continue ;;
    esac
    "$EMBCC" -c -isystem "$X86_NEWLIB/include" "$f" -o "$out/a.o" \
        2>/dev/null || continue
    if ! "$EMBCC" -S -isystem "$X86_NEWLIB/include" "$f" -o "$out/a.s" \
             2>/dev/null ||
       ! "$AS" "$out/a.s" -o "$out/b.o" 2>/dev/null; then
        bad=$((bad + 1)); echo "  FAIL (would not assemble): $f"; continue
    fi
    ta=$(readelf -x .text "$out/a.o" 2>/dev/null | tail -n +3)
    tb=$(readelf -x .text "$out/b.o" 2>/dev/null | tail -n +3)
    # Each relocation whole -- where, what kind, against what, plus what.
    # A count let -S write `t-4` where the object had `t` for every
    # thread-local access and still pass.
    ra=$(relocs "$out/a.o")
    rb=$(relocs "$out/b.o")
    if [ "$ta" = "$tb" ] && [ "$ra" = "$rb" ]; then
        ident=$((ident + 1))
    else
        bad=$((bad + 1)); echo "  FAIL (different object): $f"
    fi
done
[ "$bad" = 0 ] ||
    { echo "FAIL: $bad of $((ident + bad)) programs did not round-trip"; exit 1; }
echo "$ident execution programs: -S then $AS gives the same .text and the
same relocations as -c, every one"

# ---- the other targets now work, and are checked where they belong ---------
# This used to assert that -S REFUSED for aarch64, which was the right
# behaviour while the byte grouping came from the x86-64 disassembler.
# It no longer does: src/arch/target.c answers the instruction length
# per target and asmout.c knows each target's relocation names, so -S
# emits the object as text everywhere. That property -- reassembling to
# a byte-identical .text -- is checked for all five targets by
# tests/golden/asmout-roundtrip.sh, which needs llvm-mc rather than the
# x86-64 binutils this file uses.
if ! "$EMBCC" -S --target=aarch64-elf "$out/p.c" -o "$out/x.s" \
        > "$out/err.txt" 2>&1; then
    cat "$out/err.txt"; echo "FAIL: -S should work for aarch64 now"; exit 1
fi
grep -q '\.byte' "$out/x.s" ||
    { echo "FAIL: aarch64 -S produced no instruction bytes"; exit 1; }
# A64 is fixed 32-bit: every instruction line must carry four bytes.
# (A real tab in the pattern: GNU grep reads \t as a plain t, and this
# check could not fail on Linux.)
tab=$(printf '\t')
if grep -qE "^$tab\.byte${tab}0x..(,0x..)?\$" "$out/x.s"; then
    echo "FAIL: aarch64 -S grouped bytes into non-4-byte instructions"
    grep -nE "^$tab\.byte${tab}0x..(,0x..)?\$" "$out/x.s" | head -3; exit 1
fi
echo "-S works for aarch64 too, grouped 4 bytes to the instruction"
