#!/bin/sh
# The x86-64 decoder behind `embcc -S` and EmbDBG (src/arch/x86_64/disasm.c)
# against llvm-objdump.
#
# `-S` writes each instruction's bytes on one `.byte` line, decoded into a
# comment, so the decoder's LENGTHS are the lines. A wrong length desyncs
# every instruction after it -- the comments then describe a different
# program -- and an opcode the decoder did not know was printed as one
# `.byte` and its operands read as instructions of their own. Before this
# test, 446 of 3874 functions of tests/exec split differently from
# llvm-objdump: SSE moves, cmov, bswap, shld/shrd, the string operations
# and every x87 instruction long double uses, and a 16-bit `mov $imm`
# that swallowed the next instruction. Now they must all agree.
set -u
echo "TEST-MARKER x86-disasm"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/x86-disasm
rm -rf "${out:?}"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
command -v llvm-objdump >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 || {
    echo "SKIP: needs llvm-objdump and python3"; exit 0; }

# ---- 1. every function of the corpus splits as llvm-objdump splits it --
python3 tests/golden/x86-disasm/split.py "$EMBCC" tests/exec/*.c > "$out/split.txt" 2>&1 || {
    echo "FAIL: the decoder and llvm-objdump disagree on instruction lengths:"
    grep MISMATCH "$out/split.txt" | head -10
    tail -1 "$out/split.txt"
    exit 1; }
tail -1 "$out/split.txt"

# ---- 2. the forms it used to print as `.byte` are named ----------------
cat > "$out/forms.c" <<'CEOF'
typedef struct { long a[8]; } big;
long double ld_add(long double a, long double b) { return a + b * 2.0L; }
int pick(int c, int a, int b) { return c ? a : b; }
unsigned swap(unsigned x) { return __builtin_bswap32(x); }
void copy(big *d, const big *s) { *d = *s; }
int va[1024], vb[1024], vc[1024];
void vadd(void) { for (int i = 0; i < 1024; i++) va[i] = vb[i] + vc[i]; }
long la[512], lb[512];
void vaddq(void) { for (int i = 0; i < 512; i++) la[i] += lb[i]; }
CEOF
"$EMBCC" --target=x86_64-elf -O2 -S "$out/forms.c" -o "$out/forms.s" || {
    echo "FAIL: forms.c does not compile"; exit 1; }
for mn in fldt fstpt faddp fmulp cmovne bswap movdqu movdqa paddd paddq; do
    grep -q "# $mn " "$out/forms.s" || {
        echo "FAIL: no '$mn' in the -S comments; what it printed instead:"
        grep -c '# \.byte' "$out/forms.s"; exit 1; }
done
if grep -q '# \.byte' "$out/forms.s"; then
    echo "FAIL: -S still prints undecoded bytes:"
    grep '# \.byte' "$out/forms.s" | head -5; exit 1
fi
echo "x86-disasm: every function splits as llvm-objdump splits it, and x87, SSE2, cmov and bswap are named"
