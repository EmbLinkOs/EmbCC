#!/bin/sh
# On AArch64 ELF, a file-scope asm block's instructions and a naked
# function's body are assembled by the GNU-syntax assembler that reads its
# .s files, as on every embedded target.
#
# They went to x86-64's file-scope encoder instead, which knows a handful
# of x86 instructions: `add x0, x0, #2` in a block was refused ("EmbCC
# assembles instructions for x86-64 only"), and so was every naked
# function, although `embcc -c start.s` assembled the same lines. Into
# Mach-O and COFF, whose writers take no relocation type from an
# assembler, both are still refused, by name.
set -u
echo "TEST-MARKER aarch64-asm-blocks"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/aarch64-asm-blocks
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/b.c" <<'EOF'
__asm__(".globl add2\n"
        "add2:\n"
        "\tadd x0, x0, #2\n"
        "\tret\n");
long add2(long);
__attribute__((naked)) long twice(long x)
{
    __asm__("add x0, x0, x0\n\tret");
}
long f(long a) { return twice(add2(a)); }
int main(void) { return (int)f(19); }      /* 2 * (19 + 2) */
EOF

# what each symbol disassembles to, one instruction per line
insns() {
    llvm-objdump -d --no-show-raw-insn "$1" |
        awk -v s="<$2>:" '$0 ~ s { on = 1; next } on && /^$/ { exit }
                          on { $1 = ""; sub(/^ +/, ""); gsub(/\t/, " "); print }'
}
for t in aarch64-elf aarch64-linux-gnu; do
    for o in -O0 -O2; do
        "$EMBCC" --target=$t $o -c "$out/b.c" -o "$out/b-$t$o.o" 2> "$out/err" ||
            fail "$t $o: the block or the naked function did not compile: $(head -1 "$out/err")"
        command -v llvm-objdump > /dev/null 2>&1 || continue
        [ "$(insns "$out/b-$t$o.o" add2 | tr '\n' ';')" = "add x0, x0, #0x2;ret;" ] ||
            fail "$t $o: add2 is not the block's instructions: $(insns "$out/b-$t$o.o" add2 | tr '\n' ';')"
        [ "$(insns "$out/b-$t$o.o" twice | tr '\n' ';')" = "add x0, x0, x0;ret;" ] ||
            fail "$t $o: twice is not the naked body: $(insns "$out/b-$t$o.o" twice | tr '\n' ';')"
    done
done
echo "aarch64-elf and aarch64-linux-gnu assemble a block's instructions and a naked body, at -O0 and -O2"

# Mach-O: refused by name, each
printf '__asm__("f2:\\n\\tret\\n");\n' > "$out/blk.c"
printf '__attribute__((naked)) void n(void) { __asm__("ret"); }\n' > "$out/nk.c"
"$EMBCC" --target=aarch64-apple-darwin -c "$out/blk.c" -o "$out/x.o" 2> "$out/e1" &&
    fail "aarch64-apple-darwin compiled a block's instruction into Mach-O"
grep -q 'not yet into Mach-O objects' "$out/e1" ||
    fail "aarch64-apple-darwin's refusal of a block does not say why: $(head -1 "$out/e1")"
"$EMBCC" --target=aarch64-apple-darwin -c "$out/nk.c" -o "$out/x.o" 2> "$out/e2" &&
    fail "aarch64-apple-darwin compiled a naked function"
grep -q 'naked)) is not supported' "$out/e2" ||
    fail "aarch64-apple-darwin's refusal of naked does not say so: $(head -1 "$out/e2")"
echo "and into Mach-O both are refused by name"

# and the program runs, where the AArch64 QEMU harness links
if "$EMBCC_ROOT/tests/harness/aarch64/link.sh" -o "$out/b" "$out/b-aarch64-elf-O2.o" > "$out/lerr" 2>&1; then
    "$EMBCC_ROOT/tests/harness/aarch64/run.sh" "$out/b" > /dev/null 2>&1
    st=$?
    [ $st -eq 42 ] || fail "the program returned $st on QEMU's AArch64 board, not 42"
    echo "and the program returns 42 on QEMU's AArch64 board"
else
    echo "SKIP: the AArch64 QEMU harness does not link here: $(head -1 "$out/lerr")"
fi
