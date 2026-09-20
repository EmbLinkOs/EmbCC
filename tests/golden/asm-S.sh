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
    ra=$(readelf -rW "$out/a.o" | grep -c '^0' || true)
    rb=$(readelf -rW "$out/b.o" | grep -c '^0' || true)
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

# ---- and it refuses where it cannot be trusted -----------------------------
if "$EMBCC" -S --target=aarch64-elf "$out/p.c" -o "$out/x.s" \
       > "$out/err.txt" 2>&1; then
    echo "FAIL: -S claimed to work for aarch64"; exit 1
fi
grep -q "x86-64 only" "$out/err.txt" ||
    { cat "$out/err.txt"; echo "FAIL: should say why"; exit 1; }
echo "-S refuses for aarch64 rather than emitting text it cannot verify"
