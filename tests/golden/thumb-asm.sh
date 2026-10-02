#!/bin/sh
# Inline assembly for ARMv7-M (D-015), in the three ways it can be wrong.
#
# 1. THE ENCODINGS. tools/tasmcheck hands the assembler's whole
#    vocabulary -- generated from asm.c's own tables, so an instruction
#    added there cannot escape -- to itself, and the same lines to
#    llvm-mc. The bytes must be identical. It already caught one that
#    reading would not have: `adds` was mapped onto the same table entry
#    as `add` with the flag-setting bit clear, so a program that wrote
#    `adds` to set the flags for a following branch got an `add` that set
#    nothing. It assembled, it ran, and it took the wrong branch.
#
# 2. THE OPERANDS, by RUNNING a program whose answers depend on the
#    caller's values reaching the registers the template names, on QEMU,
#    at four optimisation levels -- the allocator is on at two of them
#    and inline asm is exactly where it must keep its hands off.
#
# 3. THE REFUSALS, by name. A vocabulary whose misses are silent is not
#    a vocabulary.
set -u
echo "TEST-MARKER thumb-asm"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
out=tests/golden/out/thumb-asm
rm -rf "$out"; mkdir -p "$out"

if command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1
then
    cc -std=c99 -Wall -Wextra -o "$out/tasmcheck" \
       tools/tasmcheck/tasmcheck.c src/arch/thumb/asm.c \
       src/arch/thumb/emit.c src/arch/code.c src/driver/util.c \
       src/driver/diag.c src/platform/platform_posix.c || {
        echo "tasmcheck did not build"; exit 1; }
    "$out/tasmcheck" --list > "$out/v.s" || {
        echo "could not list the vocabulary"; exit 1; }
    "$out/tasmcheck" bytes > "$out/v.bin" 2> "$out/v.err" || {
        echo "the assembler refused its own vocabulary:"
        head -3 "$out/v.err"; exit 1; }
    "$MC" -triple=thumbv7m-none-eabi -filetype=obj "$out/v.s" \
        -o "$out/v.o" 2> "$out/v.mc" || {
        echo "llvm-mc rejected the vocabulary -- an entry claims an"
        echo "        instruction that does not exist:"
        head -4 "$out/v.mc"; exit 1; }
    "$OBJCOPY" -O binary --only-section=.text "$out/v.o" "$out/v.ref" \
        2>/dev/null
    if ! cmp -s "$out/v.bin" "$out/v.ref"; then
        echo "an encoding differs from llvm-mc's:"
        od -An -tx1 "$out/v.bin" > "$out/ours"
        od -An -tx1 "$out/v.ref" > "$out/theirs"
        diff "$out/theirs" "$out/ours" | head -8
        exit 1
    fi
    echo "$(wc -l < "$out/v.s" | tr -d ' ') instructions encode as llvm-mc does"
else
    echo "SKIP the encoding half: llvm-mc/llvm-objcopy not found"
fi

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "SKIP the run half: $QEMU absent"
    exit 0
fi

cat > "$out/asm.c" <<'CEOF'
void writec(int c); void puts_(const char *s); void putn(long v);

static int add3(int a, int b)
{ int r; __asm__("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }

static int shl(int a)
{ int r; __asm__("lsl %[d], %[s], #3" : [d]"=r"(r) : [s]"r"(a)); return r; }

static int accum(int seed)
{ int v = seed; __asm__("add %0, %0, #7" : "+r"(v)); return v; }

/* The special registers, which is what inline asm exists for here.
 * PRIMASK is 0 with interrupts enabled, 1 with them masked -- so this
 * checks a critical section actually masks, not merely that cpsid
 * assembles. */
static int masked(void)
{ unsigned before, during, after;
  __asm__ volatile("mrs %0, primask" : "=r"(before));
  __asm__ volatile("cpsid i" ::: "memory");
  __asm__ volatile("mrs %0, primask" : "=r"(during));
  __asm__ volatile("cpsie i" ::: "memory");
  __asm__ volatile("mrs %0, primask" : "=r"(after));
  return (int)(before * 100 + during * 10 + after); }

static int fenced(int x)
{ __asm__ volatile("dsb sy; isb sy" ::: "memory"); return x + 1; }

static int chain(int a)
{ int r; __asm__("add %0, %1, #1; lsl %0, %0, #2; sub %0, %0, #3"
                 : "=r"(r) : "r"(a)); return r; }

static int bits(unsigned x)
{ unsigned r; __asm__("clz %0, %1" : "=r"(r) : "r"(x)); return (int)r; }

/* An "I" operand is a LITERAL in the template, and a register variable
 * pins its operand. Both went through x86's constraint letters, where
 * "I" was refused and a register variable matched x86's names. */
static int addk(int a)
{ int r; __asm__("adds %0, %1, %2" : "=r"(r) : "r"(a), "I"(9)); return r; }
static int pinned(int a)
{ register int y __asm__("r2") = a; int r;
  __asm__("adds %0, %1, #2" : "=r"(r) : "r"(y)); return r; }

int main(void)
{
    putn(add3(20, 22));        /* 42 */
    putn(shl(5));              /* 40 */
    putn(accum(35));           /* 42 */
    putn(masked());            /* 0, then 1, then 0 -> 010 -> 10 */
    putn(fenced(41));          /* 42 */
    putn(chain(10));           /* (10+1)<<2 - 3 = 41 */
    putn(bits(1u << 20));      /* 11 leading zeros */
    putn(addk(33));            /* 42 */
    putn(pinned(40));          /* 42 */
    puts_("\n==END==\n");
    return 0;
}
CEOF

T=thumbv7m-none-eabi
export EMBCC_THUMB_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -c "tests/harness/thumb/$f.c" -o "$out/$f.o" ||
        { echo "the harness does not compile"; exit 1; }
done
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c "$out/asm.c" -o "$out/a$opt.o" ||
        { echo "$opt: the asm program does not compile"; exit 1; }
    sh tests/harness/thumb/link.sh "$out/a$opt.elf" "$out/a$opt.o" ||
        { echo "$opt: could not link"; exit 1; }
    sh tests/harness/thumb/run.sh "$out/a$opt.elf" > "$out/a$opt.txt" 2>&1
    got=$(tr -d '\n' < "$out/a$opt.txt" | sed 's/==END==.*//')
    want="42 40 42 10 42 41 11 42 42 "
    [ "$got" = "$want" ] || {
        echo "$opt: inline asm computed '$got', wanted '$want'"; exit 1; }
done
echo "inline asm computes correctly at -O0, -O1, -O2 and -Os"

# An instruction outside the vocabulary fails by NAME, never with a
# guessed encoding.
printf 'void f(void){ __asm__("vldr s0, [r0]"); }\n' > "$out/bad.c"
if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
    echo "an unknown instruction was accepted"; exit 1
fi
grep -q "vldr" "$out/bad.err" || {
    echo "the refusal does not name the instruction:"; cat "$out/bad.err"
    exit 1; }
# And a callee-saved register is refused rather than silently corrupted:
# EmbCC saves nothing around an asm.
printf 'void f(void){ __asm__("mov r5, r0" ::: "r5"); }\n' > "$out/cs.c"
if "$EMBCC" --target=$T -c "$out/cs.c" -o /dev/null 2> "$out/cs.err"; then
    echo "a callee-saved clobber was accepted"; exit 1
fi
grep -q "callee-saved" "$out/cs.err" || {
    echo "the refusal does not say why:"; cat "$out/cs.err"; exit 1; }
echo "an unknown instruction and a callee-saved clobber are refused by name"

# x86's constraint letters mean nothing here: "S" pinned the operand to
# x86 register 6, r6, which is callee-saved and was not saved; and a
# register variable on r4-r11 would be loaded without being saved.
printf 'int f(int x){ int r; __asm__("mov %%0, %%1" : "=r"(r) : "S"(x)); return r; }\n' \
    > "$out/x86c.c"
if "$EMBCC" --target=$T -c "$out/x86c.c" -o /dev/null 2> "$out/x86c.err"; then
    echo "x86's \"S\" was accepted on ARMv7-M"; exit 1
fi
grep -q 'asm constraint "S" is not valid for ARMv7-M' "$out/x86c.err" || {
    echo "the \"S\" refusal is not by name:"; cat "$out/x86c.err"; exit 1; }
printf 'int f(int x){ register int y __asm__("r6") = x; int r; __asm__("mov %%0, %%1" : "=r"(r) : "r"(y)); return r; }\n' \
    > "$out/r6.c"
if "$EMBCC" --target=$T -c "$out/r6.c" -o /dev/null 2> "$out/r6.err"; then
    echo "a register variable on callee-saved r6 was accepted"; exit 1
fi
echo "x86's constraint letters and a callee-saved register variable are refused"
