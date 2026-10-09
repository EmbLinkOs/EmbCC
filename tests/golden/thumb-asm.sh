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
#    Its data directives (.byte, .short, .word, .quad and the other GNU
#    as ARM spellings) are in it too, and the same lines in a C template
#    must come out of the compiler as llvm-mc's bytes at ARMv7E-M,
#    ARMv6-M and ARMv8-M Baseline -- on the last two, a .short holding a
#    Thumb-2 halfword is data, not an instruction those cores lack.
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
       src/arch/thumb/emit.c src/arch/thumb/a32.c src/arch/code.c src/driver/util.c \
       src/driver/diag.c src/platform/platform_common.c src/platform/platform_posix.c || {
        echo "tasmcheck did not build"; exit 1; }
    "$out/tasmcheck" --list > "$out/v.s" || {
        echo "could not list the vocabulary"; exit 1; }
    "$out/tasmcheck" bytes > "$out/v.bin" 2> "$out/v.err" || {
        echo "the assembler refused its own vocabulary:"
        head -3 "$out/v.err"; exit 1; }
    "$MC" -triple=thumbv7em-none-eabi -mattr=+vfp4d16sp -filetype=obj "$out/v.s" \
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

    # the data directives in a C template, through the compiler, at
    # each level the assembler serves: llvm-mc's bytes, in the function
    grep -E '^[[:space:]]+\.(byte|short|hword|2byte|word|long|int|4byte|quad|8byte) ' \
        "$out/v.s" > "$out/data.s"
    [ "$(wc -l < "$out/data.s")" -ge 10 ] || {
        echo "the vocabulary has no data directives"; exit 1; }
    { printf 'void f(void)\n{\n    __asm__ volatile(\n'
      sed 's/^[[:space:]]*/        "/; s/$/\\n"/' "$out/data.s"
      printf '    );\n}\n'; } > "$out/data.c"
    hex() { od -An -tx1 "$1" | tr -d ' \n'; }
    for t in thumbv7em thumbv6m thumbv8m.base; do
        "$EMBCC" --target=$t-none-eabi -O2 -c "$out/data.c" -o "$out/data-$t.o" 2> "$out/data-$t.err" || {
            echo "$t: a template of data does not compile:"; head -2 "$out/data-$t.err"; exit 1; }
        "$OBJCOPY" -O binary --only-section=.text "$out/data-$t.o" "$out/data-$t.bin"
        "$MC" -triple=$t -filetype=obj "$out/data.s" -o "$out/data-$t.ref.o" &&
        "$OBJCOPY" -O binary --only-section=.text "$out/data-$t.ref.o" "$out/data-$t.ref" || {
            echo "$t: llvm-mc rejected the data directives"; exit 1; }
        case $(hex "$out/data-$t.bin") in
        *"$(hex "$out/data-$t.ref")"*) ;;
        *) echo "$t: the template's data is not llvm-mc's bytes:"
           echo "     | ours   $(hex "$out/data-$t.bin")"
           echo "     | theirs $(hex "$out/data-$t.ref")"; exit 1 ;;
        esac
    done
    echo "data directives in a template are llvm-mc's bytes at ARMv7E-M, ARMv6-M and ARMv8-M Baseline"

    # Strings, alignments and %c (tests/harness/asmdir.sh): .ascii,
    # .asciz/.string, .p2align/.balign/.align with and without a fill and
    # a maximum, and constants written in with %c0 -- Linux's asm-offsets
    # and EmbLinkRTOS's layout probes. llvm-mc's bytes at every halfword
    # phase in the section: the padding is decided where the template
    # lands, and a template padded as if it started on 16 fails at seven
    # of the eight. ARM state's nops are words, so it gets its own run.
    . tests/harness/asmdir.sh
    cat > "$out/dirs.txt" <<'EOF'
.ascii "->EMB_PROBE s %c1 %c0"
.p2align 2
.ascii "abc"
.align 3
.asciz "hi", "x"
.byte 0x55
.p2align 4
.string "\t\"q\\\101\x42\0z"
.balign 8
.byte 1, 255, -128, %c0, %c1
.p2align 3, 0x5a
.byte 7
.p2align 4,,5
.byte 9
.p2align 4,,15
.ascii "a;b@c#d//e"
.hword 0x1234, %c0
.word 0x12345678
.quad -2
.p2align 2
EOF
    asmdir_referee "$out" thumbv7em-none-eabi "-triple=thumbv7em" nop 2 \
        "$out/dirs.txt" || exit 1
    asmdir_referee "$out" armv7a-none-eabi "-triple=armv7a" nop 4 \
        "$out/dirs.txt" || exit 1
    # ARMv6-M and ARMv8-M Baseline copy a template as it is (v6m.c), so
    # only what pads the same anywhere -- to two -- is theirs yet
    { grep -v 'align' "$out/dirs.txt"; echo '.ascii "q"'; echo '.p2align 1'
      echo '.byte 3'; echo '.balign 2, 0x77'; } > "$out/dirs1.txt"
    for t in thumbv6m thumbv8m.base; do
        asmdir_referee "$out" $t-none-eabi "-triple=$t" nop 2 \
            "$out/dirs1.txt" || exit 1
    done
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

/* An "m" output: its register holds the ADDRESS the template writes
 * through, and nothing loaded it -- the store went wherever the register
 * last pointed. */
static int memout(int k)
{ int v = 0; __asm__ volatile("str %1, [%0]" : "=m"(v) : "r"(k)); return v; }

/* Instructions written as their bytes: adds r0, #1 as a .short, adds r0,
 * #2 as two .bytes, and adds r0, #2 then adds r0, #1 as one .4byte, on
 * the r0 the register variable pins. The data directives were refused,
 * "not in the ARMv7-M vocabulary". */
static int raw(int a)
{ register int x __asm__("r0") = a;
  __asm__(".short 0x3001\n\t.byte 0x02, 0x30; .4byte 0x30013002"
          : "+r"(x) : : "cc"); return x; }

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
    putn(memout(42));          /* 42 */
    putn(raw(36));             /* 36 + 1 + 2 + 2 + 1 = 42 */
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
    want="42 40 42 10 42 41 11 42 42 42 42 "
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

# Inline data is constants, in range: a symbol would need a relocation an
# inline asm cannot carry, a value too wide for its directive is an error
# in GNU as and llvm-mc alike, RISC-V's .half is no ARM directive, and an
# IT block's slots are for instructions.
for d in ".word handler" ".byte 256" ".short -32769" ".word 0x100000000" \
         ".half 1" "it eq; .short 0xbf00"; do
    printf 'void f(void){ __asm__ volatile("%s"); }\n' "$d" > "$out/data.c"
    if "$EMBCC" --target=$T -c "$out/data.c" -o /dev/null 2> "$out/data.err"; then
        echo "inline asm accepted: $d"; exit 1
    fi
    grep -q 'is not a constant\|does not fit in\|not in the ARMv7-M vocabulary\|inside an IT block' "$out/data.err" || {
        echo "the refusal of '$d' does not say why:"; cat "$out/data.err"; exit 1; }
done
printf 'void f(void){ __asm__ volatile(".byte -128, 255; .short -32768, 65535; .quad -1"); }\n' > "$out/data.c"
"$EMBCC" --target=$T -c "$out/data.c" -o /dev/null || {
    echo "inline asm refused data at the ends of its range"; exit 1; }
echo "inline data: a symbol, an out-of-range value, .half and data in an IT block are refused"

# %c prints a constant, and a register operand is not one; a string or an
# alignment is not an instruction an IT block's condition can apply to;
# and on ARMv6-M an alignment the template cannot settle by itself is
# refused, where padding it for the wrong place would be silent.
printf 'int f(int x){ __asm__ volatile(".byte %%c0" : : "r"(x)); return x; }\n' > "$out/c.c"
if "$EMBCC" --target=$T -c "$out/c.c" -o /dev/null 2> "$out/c.err"; then
    echo "%c of a register operand was accepted"; exit 1
fi
grep -q "names a register operand" "$out/c.err" || {
    echo "the refusal of %c on a register does not say why:"; cat "$out/c.err"; exit 1; }
printf 'void f(void){ __asm__ volatile("it eq; .ascii \\"ab\\""); }\n' > "$out/c.c"
if "$EMBCC" --target=$T -c "$out/c.c" -o /dev/null 2> "$out/c.err"; then
    echo "a string inside an IT block was accepted"; exit 1
fi
grep -q "inside an IT block" "$out/c.err" || {
    echo "the refusal of a string in an IT block does not say why:"; cat "$out/c.err"; exit 1; }
printf 'void f(void){ __asm__ volatile(".ascii \\"abc\\"; .p2align 2"); }\n' > "$out/c.c"
for t in thumbv6m thumbv8m.base; do
    if "$EMBCC" --target=$t-none-eabi -c "$out/c.c" -o /dev/null 2> "$out/c.err"; then
        echo "$t: an alignment to four was accepted, and padded for nowhere"; exit 1
    fi
    grep -q "does not do yet" "$out/c.err" || {
        echo "$t: the refusal of .p2align 2 does not say why:"; cat "$out/c.err"; exit 1; }
done
# more than .text's sixteen, a byte count that is no power of two, an
# escape no assembler reads, a string that is not one
cat > "$out/c.c" <<'CEOF'
void a(void) { __asm__ volatile(".p2align 5"); }
CEOF
cat > "$out/c2.c" <<'CEOF'
void a(void) { __asm__ volatile(".balign 3"); }
CEOF
cat > "$out/c3.c" <<'CEOF'
void a(void) { __asm__ volatile(".ascii \"\\q\""); }
CEOF
cat > "$out/c4.c" <<'CEOF'
void a(void) { __asm__ volatile(".asciz 5"); }
CEOF
for c in c c2 c3 c4; do
    if "$EMBCC" --target=$T -c "$out/$c.c" -o /dev/null 2> "$out/c.err"; then
        echo "inline asm accepted:"; cat "$out/$c.c"; exit 1
    fi
    grep -q 'at most 16\|not a power of two\|not an escape\|not a quoted string' "$out/c.err" || {
        echo "the refusal of $(cat "$out/$c.c") does not say why:"; cat "$out/c.err"; exit 1; }
done
echo "%c on a register, a string in an IT block, .p2align 2 on ARMv6-M, more than 16, a bad escape are refused"

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

# `movs` and `mvns` SET the flags -- a branch after them in the same
# template reads them -- and were encoded as movw/mov and mvn.w, which do
# not. A movs immediate no flag-setting MOV can hold is refused.
printf 'int f(int x){ int r; __asm__("movs %%0, #0\\n mvns %%0, %%1\\n movs %%0, %%1\\n movs %%0, #0x10000" : "=r"(r) : "r"(x)); return r; }\n' \
    > "$out/flags.c"
"$EMBCC" --target=$T -c "$out/flags.c" -o "$out/flags.o" || {
    echo "the flag-setting moves do not assemble"; exit 1; }
fl=$(llvm-objdump -d --triple=thumbv7em "$out/flags.o" | grep -cE '[[:space:]](movs(\.w)?|mvns)[[:space:]]')
[ "$fl" = 4 ] || {
    echo "$fl of 4 flag-setting moves set the flags:"
    llvm-objdump -d --triple=thumbv7em "$out/flags.o" | grep -E 'mov|mvn'; exit 1; }
printf 'int f(int x){ int r; __asm__("movs %%0, #0x12345" : "=r"(r) : "r"(x)); return r; }\n' \
    > "$out/movs-big.c"
if "$EMBCC" --target=$T -c "$out/movs-big.c" -o /dev/null 2>/dev/null; then
    echo "movs of an immediate no MOVS encodes was accepted"; exit 1
fi
echo "movs and mvns set the flags, or are refused"

# Data in a template is data in the object: a $d mapping symbol where it
# starts and $t where the code resumes, on ARMv7-M as on ARMv6-M -- or a
# disassembler and a debugger read the word as instructions.
printf 'int raw(int x){ __asm__ volatile("adds %%0, #1\\n .word 0x12345678\\n adds %%0, #2" : "+r"(x)); return x; }\n' \
    > "$out/dmap.c"
for tt in thumbv7em-none-eabi thumbv6m-none-eabi; do
    "$EMBCC" --target=$tt -O2 -c "$out/dmap.c" -o "$out/dmap.o" || {
        echo "$tt: the data template does not compile"; exit 1; }
    llvm-objdump -d "$out/dmap.o" > "$out/dmap.dis"
    grep -q '\.word.*0x12345678' "$out/dmap.dis" || {
        echo "$tt: the template's .word is not marked as data:"
        cat "$out/dmap.dis"; exit 1; }
    grep -qE 'adds[[:space:]]+r[0-7], #0x2' "$out/dmap.dis" || {
        echo "$tt: the code after the template's data is not marked as code:"
        cat "$out/dmap.dis"; exit 1; }
done
echo "a template's data is \$d in the object and the code after it \$t, on ARMv7-M and ARMv6-M"
