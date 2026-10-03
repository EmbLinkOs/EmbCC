#!/bin/sh
# Inline assembly for RISC-V (D-016), in the two ways it can be wrong.
#
# 1. THE ENCODINGS. tools/rvasmcheck hands the assembler's whole
#    vocabulary -- generated from asm.c's own tables, so an instruction
#    added there cannot escape -- to itself, and the same lines to
#    llvm-mc. The bytes must be identical. A hand-written assembler with
#    no referee is a guess, and this one already caught two: a bare
#    `fence` emitted as `fence rw, rw` (which orders memory but not
#    device I/O, so a barrier written for an MMIO register would not have
#    ordered it), and four CSRs that exist only on RV32.
#
# 2. THE OPERANDS. Encoding the template right is half of it; the other
#    half is putting the caller's values in the registers the template
#    names and getting the results back out. That is checked by RUNNING a
#    program whose answers depend on it, on QEMU, at four optimisation
#    levels -- because the register allocator is on at two of them and
#    inline asm is exactly where it must keep its hands off.
set -u
echo "TEST-MARKER riscv-asm"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
out=tests/golden/out/riscv-asm
rm -rf "$out"; mkdir -p "$out"

# ---- 1. the vocabulary, against llvm-mc --------------------------------
if command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1
then
    cc -std=c99 -Wall -Wextra -o "$out/rvasmcheck" \
       tools/rvasmcheck/rvasmcheck.c src/arch/riscv/asm.c \
       src/arch/riscv/emit.c src/arch/code.c src/arch/target.c \
       src/driver/util.c src/driver/diag.c src/platform/platform_posix.c \
       src/sema/type.c src/sema/ldfloat.c || {
        echo "rvasmcheck did not build"; exit 1; }

    for w in 64 32; do
        tag=rv$w
        "$out/rvasmcheck" --list "$tag" > "$out/$tag.s" || {
            echo "$tag: could not list the vocabulary"; exit 1; }
        "$out/rvasmcheck" bytes "$tag" > "$out/$tag.bin" 2> "$out/$tag.err" || {
            echo "$tag: the assembler refused its own vocabulary:"
            head -3 "$out/$tag.err"; exit 1; }
        "$MC" -triple="riscv$w" -mattr=+m -filetype=obj "$out/$tag.s" \
            -o "$out/$tag.o" 2> "$out/$tag.mc" || {
            echo "$tag: llvm-mc rejected the vocabulary -- an entry claims an"
            echo "        instruction that does not exist:"
            head -4 "$out/$tag.mc"; exit 1; }
        "$OBJCOPY" -O binary --only-section=.text "$out/$tag.o" \
            "$out/$tag.ref" 2>/dev/null
        if ! cmp -s "$out/$tag.bin" "$out/$tag.ref"; then
            echo "$tag: an encoding differs from llvm-mc's:"
            od -An -tx1 "$out/$tag.bin" > "$out/$tag.ours"
            od -An -tx1 "$out/$tag.ref" > "$out/$tag.theirs"
            diff "$out/$tag.theirs" "$out/$tag.ours" | head -8
            exit 1
        fi
        echo "$tag: $(wc -l < "$out/$tag.s" | tr -d ' ') instructions encode as llvm-mc does"
    done
else
    echo "SKIP the encoding half: llvm-mc/llvm-objcopy not found"
fi

# ---- 2. the operands, by running it ------------------------------------
cat > "$out/asm.c" <<'CEOF'
void writec(int c); void puts_(const char *s); void putn(long v);

/* Plain operands: two inputs, one output. */
static int add3(int a, int b)
{ int r; __asm__("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }

/* A named operand, and an immediate one folded into the template. */
static int shl(int a)
{ int r; __asm__("slli %[d], %[s], 3" : [d]"=r"(r) : [s]"r"(a)); return r; }

/* A "+" output: the register must arrive holding the lvalue's value. */
static int accum(int seed)
{ int v = seed; __asm__("addi %0, %0, 7" : "+r"(v)); return v; }

/* A CSR read -- the reason inline asm exists on this target at all.
 * mhartid is 0 on QEMU's single-hart virt board. */
static long hart(void)
{ long v; __asm__ volatile("csrr %0, mhartid" : "=r"(v)); return v; }

/* A barrier, which emits an instruction and clobbers memory. */
static int fenced(int x)
{ __asm__ volatile("fence" ::: "memory"); return x + 1; }

/* Several statements in one template, separated as an assembler does. */
static int chain(int a)
{ int r; __asm__("addi %0, %1, 1; slli %0, %0, 2; addi %0, %0, -3"
                 : "=r"(r) : "r"(a)); return r; }

/* An "i" operand is a LITERAL in the template, and a register variable
 * pins its operand. Both went through x86's constraint letters, where
 * "i" became a register and a register variable an x86 name. */
static int addk(int a)
{ int r; __asm__("addi %0, %1, %2" : "=r"(r) : "r"(a), "i"(9)); return r; }
static int pinned(int a)
{ register int y __asm__("a3") = a; int r;
  __asm__("addi %0, %1, 2" : "=r"(r) : "r"(y)); return r; }

/* An "m" output: its register holds the ADDRESS the template writes
 * through, and nothing loaded it -- the store went wherever the register
 * last pointed. */
static int memout(int k)
{ int v = 0; __asm__ volatile("sw %1, 0(%0)" : "=m"(v) : "r"(k)); return v; }

int main(void)
{
    putn(add3(20, 22));        /* 42 */
    putn(shl(5));              /* 40 */
    putn(accum(35));           /* 42 */
    putn(hart());              /* 0 */
    putn(fenced(41));          /* 42 */
    putn(chain(10));           /* (10+1)<<2 - 3 = 41 */
    putn(addk(33));            /* 42 */
    putn(pinned(40));          /* 42 */
    putn(memout(42));          /* 42 */
    puts_("\n==END==\n");
    return 0;
}
CEOF

ran=0
for w in 32 64; do
    QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv$w}
    command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP rv$w: $QEMU absent"; continue; }
    ran=$((ran + 1))
    T=riscv$w-unknown-elf
    d="$out/run$w"; mkdir -p "$d"
    export EMBCC_RISCV_HARNESS="$PWD/$d"
    for f in boot io; do
        "$EMBCC" --target=$T -c "tests/harness/riscv/$f.c" -o "$d/$f.o" ||
            { echo "rv$w: the harness does not compile"; exit 1; }
    done
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$out/asm.c" -o "$d/a$opt.o" ||
            { echo "rv$w $opt: the asm program does not compile"; exit 1; }
        sh tests/harness/riscv/link.sh "$d/a$opt.elf" "$d/a$opt.o" ||
            { echo "rv$w $opt: could not link"; exit 1; }
        sh tests/harness/riscv/run.sh "$d/a$opt.elf" "$w" > "$d/a$opt.txt" 2>&1
        # The answers are all 42 but one, and they are checked as a whole
        # line so a single wrong operand fails rather than averaging out.
        got=$(tr -d '\n' < "$d/a$opt.txt" | sed 's/==END==.*//')
        want="42 40 42 0 42 41 42 42 42 "
        [ "$got" = "$want" ] || {
            echo "rv$w $opt: inline asm computed '$got', wanted '$want'"
            exit 1; }
    done
    echo "rv$w: inline asm computes correctly at -O0, -O1, -O2 and -Os"
done

[ "$ran" -gt 0 ] || echo "SKIP the run half: no qemu-system-riscv32/64"

# ---- 3. what it REFUSES ------------------------------------------------
# An instruction outside the vocabulary must fail by NAME, never with a
# guessed encoding -- which is the whole reason to have a vocabulary.
printf 'void f(void){ __asm__("vsetvli t0, a0, e8"); }\n' > "$out/bad.c"
if "$EMBCC" --target=riscv64-unknown-elf -c "$out/bad.c" -o /dev/null \
     2> "$out/bad.err"; then
    echo "an unknown instruction was accepted"; exit 1
fi
grep -q "vsetvli" "$out/bad.err" || {
    echo "the refusal does not name the instruction:"; cat "$out/bad.err"
    exit 1; }
# And an RV32-only CSR must be refused at RV64 rather than encoded.
printf 'unsigned f(void){ unsigned v; __asm__("csrr %%0, cycleh" : "=r"(v)); return v; }\n' \
    > "$out/csr.c"
if "$EMBCC" --target=riscv64-unknown-elf -c "$out/csr.c" -o /dev/null \
     2> "$out/csr.err"; then
    echo "cycleh was accepted at RV64, where it does not exist"; exit 1
fi
"$EMBCC" --target=riscv32-unknown-elf -c "$out/csr.c" -o /dev/null || {
    echo "cycleh was refused at RV32, where it does exist"; exit 1; }
echo "an unknown instruction and an RV32-only CSR are each refused by name"

# x86's constraint letters mean nothing here. "=a" pinned the output to
# x86 register 0 -- x0, the zero register -- and the result was lost
# with no diagnostic; a register variable on a callee-saved register
# would be loaded without being saved.
printf 'int f(int x){ int r; __asm__("mv %%0, %%1" : "=a"(r) : "r"(x)); return r; }\n' \
    > "$out/x86c.c"
if "$EMBCC" --target=riscv32-unknown-elf -c "$out/x86c.c" -o /dev/null \
     2> "$out/x86c.err"; then
    echo "x86's \"=a\" was accepted on RISC-V"; exit 1
fi
grep -q 'asm constraint "=a" is not valid for RISC-V' "$out/x86c.err" || {
    echo "the \"=a\" refusal is not by name:"; cat "$out/x86c.err"; exit 1; }
printf 'int f(int x){ register int y __asm__("s0") = x; int r; __asm__("mv %%0, %%1" : "=r"(r) : "r"(y)); return r; }\n' \
    > "$out/s0.c"
if "$EMBCC" --target=riscv32-unknown-elf -c "$out/s0.c" -o /dev/null \
     2> "$out/s0.err"; then
    echo "a register variable on callee-saved s0 was accepted"; exit 1
fi
echo "x86's constraint letters and a callee-saved register variable are refused"

# RV64-only instructions are refused at RV32 rather than encoded: negw
# and sext.w were emitted with their RV64 encodings, illegal instructions
# on an RV32 part, and lwu was quietly assembled as lw.
for ins in "negw %%0, %%1" "sext.w %%0, %%1" "lwu %%0, 0(%%1)"; do
    printf 'int f(int *x){ int r; __asm__("'"$ins"'" : "=r"(r) : "r"(x)); return r; }\n' \
        > "$out/rv64only.c"
    if "$EMBCC" --target=riscv32-unknown-elf -c "$out/rv64only.c" -o /dev/null \
         2> "$out/rv64only.err"; then
        echo "RV32 accepted the RV64 instruction in: $ins"; exit 1
    fi
    grep -q "is an RV64 instruction and this is RV32" "$out/rv64only.err" || {
        echo "the refusal does not say why:"; cat "$out/rv64only.err"; exit 1; }
    "$EMBCC" --target=riscv64-unknown-elf -c "$out/rv64only.c" -o /dev/null || {
        echo "RV64 refused its own instruction in: $ins"; exit 1; }
done
echo "and RV64-only instructions are refused at RV32"
