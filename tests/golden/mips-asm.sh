#!/bin/sh
# MIPS inline assembly: the vocabulary against llvm-mc, programs that use
# it on the board, and what is refused.
#
#  1. tools/mipsasmcheck assembles every entry of src/arch/mips/asm.c's
#     vocabulary -- generated from its own tables -- and the bytes must
#     equal llvm-mc's for the same lines.
#  2. A program of asm statements (operands by number and by name, "+r",
#     "m", "i", a register variable, a multi-statement template, mfc0 of
#     coprocessor 0) runs on QEMU's malta board at -O0..-Os.
#  3. An instruction outside the vocabulary, an assembler MACRO it does
#     not expand, x86's constraint letters and a callee-saved register
#     are each refused by name.
#
# All of it again BIG-endian (mips-none-elf, llvm-mc's mips-unknown-elf,
# qemu-system-mips) as tests/golden/mips-be-asm.sh, which sets MIPS_BE=1.
set -u
if [ "${MIPS_BE:-0}" = 1 ]; then
    NAME=mips-be-asm T=mips-none-elf MT=mips-unknown-elf BYTES=bytes-be
    QEMU=${EMBCC_QEMU_MIPSEB:-qemu-system-mips}
else
    NAME=mips-asm T=mipsel-none-elf MT=mipsel-unknown-elf BYTES=bytes
    QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
fi
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out"

# ---- 1. the vocabulary ---------------------------------------------------
if command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 &&
   "$MC" -triple=$MT -mcpu=mips32r2 /dev/null -o /dev/null \
       2>/dev/null
then
    cc -std=c99 -Wall -Wextra -o "$out/mipsasmcheck" \
       tools/mipsasmcheck/mipsasmcheck.c src/arch/mips/asm.c \
       src/arch/mips/emit.c src/arch/code.c src/arch/target.c \
       src/driver/util.c src/driver/diag.c src/platform/platform_common.c \
       src/platform/platform_posix.c src/sema/type.c src/sema/ldfloat.c || {
        echo "mipsasmcheck did not build"; exit 1; }
    "$out/mipsasmcheck" --list > "$out/v.s" || {
        echo "could not list the vocabulary"; exit 1; }
    "$out/mipsasmcheck" $BYTES > "$out/v.bin" 2> "$out/v.err" || {
        echo "the assembler refused its own vocabulary:"
        head -3 "$out/v.err"; exit 1; }
    { printf '.set noreorder\n.set noat\n'; cat "$out/v.s"; } > "$out/v2.s"
    "$MC" -triple=$MT -mcpu=mips32r2 -mattr=+soft-float \
        -filetype=obj "$out/v2.s" -o "$out/v.o" 2> "$out/v.mc" || {
        echo "llvm-mc rejected the vocabulary -- an entry claims an"
        echo "instruction that does not exist:"
        head -4 "$out/v.mc"; exit 1; }
    "$OBJCOPY" -O binary --only-section=.text "$out/v.o" "$out/v.ref" \
        2>/dev/null
    if ! cmp -s "$out/v.bin" "$out/v.ref"; then
        echo "an encoding differs from llvm-mc's (llvm-mc, then ours):"
        od -An -tx4 -v "$out/v.ref" | tr -s ' ' '\n' | sed '/^$/d' \
            > "$out/theirs"
        od -An -tx4 -v "$out/v.bin" | tr -s ' ' '\n' | sed '/^$/d' \
            > "$out/ours"
        diff "$out/theirs" "$out/ours" | head -8
        exit 1
    fi
    echo "$(wc -l < "$out/v.s" | tr -d ' ') asm statements encode as llvm-mc does"
else
    echo "SKIP the encoding half: no llvm-mc with a MIPS target"
fi

# ---- 2. on the board -----------------------------------------------------
cat > "$out/asm.c" <<'CEOF'
void writec(int c); void puts_(const char *s); void putn(long v);

/* Plain operands: two inputs, one output. */
static int add3(int a, int b)
{ int r; __asm__("addu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }

/* A named operand. */
static int shl(int a)
{ int r; __asm__("sll %[d], %[s], 3" : [d]"=r"(r) : [s]"r"(a)); return r; }

/* A "+" output: the register arrives holding the lvalue's value. */
static int accum(int seed)
{ int v = seed; __asm__("addiu %0, %0, 7" : "+r"(v)); return v; }

/* Coprocessor 0: PRId's company field (bits 23:16) is 1, MIPS
 * Technologies, on QEMU's 24Kc. */
static int company(void)
{ unsigned v; __asm__ volatile("mfc0 %0, $15" : "=r"(v)); return (int)(v >> 16 & 0xff); }

/* A barrier, which emits an instruction and clobbers memory. */
static int fenced(int x)
{ __asm__ volatile("sync" ::: "memory"); return x + 1; }

/* Several statements in one template; a Release 2 bit field extract. */
static int chain(int a)
{ int r; __asm__("addiu %0, %1, 1; sll %0, %0, 2; addiu %0, %0, -3"
                 : "=r"(r) : "r"(a)); return r; }
static int field(unsigned w)
{ int r; __asm__("ext %0, %1, 8, 6" : "=r"(r) : "r"(w)); return r; }

/* An "i" operand is a literal in the template; a register variable pins
 * its operand. */
static int addk(int a)
{ int r; __asm__("addiu %0, %1, %2" : "=r"(r) : "r"(a), "i"(9)); return r; }
static int pinned(int a)
{ register int y __asm__("$a3") = a; int r;
  __asm__("addiu %0, %1, 2" : "=r"(r) : "r"(y)); return r; }

/* An "m" output: its register holds the ADDRESS the template writes. */
static int memout(int k)
{ int v = 0; __asm__ volatile("sw %1, 0(%0)" : "=m"(v) : "r"(k)); return v; }

/* A branch over one instruction, its delay slot the template's own
 * (.set noreorder)... */
static int skip(int a)
{ int r; __asm__(".set noreorder; move %0, %1; b 8; nop; addiu %0, %0, 100;"
                 " addiu %0, %0, 2" : "=r"(r) : "r"(a)); return r; }

/* ...and in the mode a template starts in, GCC's and clang's .set
 * reorder: the assembler's nop is the slot, so the addiu after the branch
 * is skipped, not executed in it. */
static int reorder(int a)
{ int r; __asm__("move %0, %1; b 8; addiu %0, %0, 100; addiu %0, %0, 2"
                 : "=r"(r) : "r"(a)); return r; }

int main(void)
{
    putn(add3(20, 22));        /* 42 */
    putn(shl(5));              /* 40 */
    putn(accum(35));           /* 42 */
    putn(company());           /* 1 */
    putn(fenced(41));          /* 42 */
    putn(chain(10));           /* (10+1)<<2 - 3 = 41 */
    putn(field(0x2a00));       /* 42 */
    putn(addk(33));            /* 42 */
    putn(pinned(40));          /* 42 */
    putn(memout(42));          /* 42 */
    putn(skip(40));            /* 42 */
    putn(reorder(40));         /* 42 */
    puts_("\n==END==\n");
    return 0;
}
CEOF

if command -v "$QEMU" >/dev/null 2>&1; then
    export EMBCC_MIPS_HARNESS="$PWD/$out"
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c "tests/harness/mips/$f.c" -o "$out/$f.o" ||
            { echo "the harness does not compile"; exit 1; }
    done
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$out/asm.c" -o "$out/a$opt.o" ||
            { echo "$opt: the asm program does not compile"; exit 1; }
        sh tests/harness/mips/link.sh "$out/a$opt.elf" "$out/a$opt.o" ||
            { echo "$opt: could not link"; exit 1; }
        sh tests/harness/mips/run.sh "$out/a$opt.elf" > "$out/a$opt.txt" 2>&1
        got=$(head -1 "$out/a$opt.txt")
        want="42 40 42 1 42 41 42 42 42 42 42 42 "
        [ "$got" = "$want" ] || {
            echo "$opt: inline asm computed '$got', wanted '$want'"; exit 1; }
    done
    echo "inline asm computes correctly on the board at -O0, -O1, -O2 and -Os"
else
    echo "SKIP the run half: $QEMU not found"
fi

# ---- 3. what is refused, by name -------------------------------------------
refuse() {          # refuse WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refuse "an instruction outside the vocabulary" '"madd' \
    'void f(int a, int b){ __asm__("madd %0, %1" :: "r"(a), "r"(b)); }'
# neg is gas's TRAPPING sub; div rs, rt with two registers is its macro
# with a divide-by-zero trap. Neither is what the bare encoding means.
refuse "neg, an assembler alias for the trapping sub" '"neg' \
    'int f(int a){ int r; __asm__("neg %0, %1" : "=r"(r) : "r"(a)); return r; }'
refuse "div with two registers, an assembler macro" 'assembler macro' \
    'void f(int a, int b){ __asm__("div %0, %1" :: "r"(a), "r"(b)); }'
refuse "x86's \"=a\" constraint" 'asm constraint "=a" is not valid for MIPS' \
    'int f(int x){ int r; __asm__("move %0, %1" : "=a"(r) : "r"(x)); return r; }'
refuse "a register variable on callee-saved s0" 'not supported for MIPS asm' \
    'int f(int x){ register int y __asm__("$s0") = x; int r; __asm__("move %0, %1" : "=r"(r) : "r"(y)); return r; }'
refuse "a template that writes callee-saved s1" "register '\$s1'" \
    'void f(void){ __asm__ volatile("move $s1, $zero"); }'
refuse "an addiu immediate beyond 16 bits" 'does not fit' \
    'int f(int a){ int r; __asm__("addiu %0, %1, 40000" : "=r"(r) : "r"(a)); return r; }'
echo "an unknown instruction, gas's neg and two-operand div, x86 constraints,"
echo "callee-saved registers and an oversized immediate are refused by name"

# ---- 4. -S ---------------------------------------------------------------
# The assembly -S writes must be the object -c writes. llvm-mc assembles
# it, and the two objects are compared as PROGRAMS: each is linked at the
# same addresses with the same harness, and the images' bytes must be
# equal. Not the objects' bytes: an o32 REL relocation keeps its addend
# in the field, and llvm-mc relocates a static symbol against its section
# (the offset in the field) where EmbCC names the symbol (zero there) --
# the same address spelled two ways, which the images see through. The
# relocations are compared too: the same fields must carry the same
# types (their targets are what the images check). The
# programs: one with calls, globals, strings and data pointers;
# tests/exec/far-switch.c at -O0, whose branches reach past 128 KiB and
# become a `j` relocated against the function's own section; and
# tests/golden/mips-exc/main.c, with a file-scope asm function and a
# naked one, whose labels and relocations -S writes as the blocks'.
OBJDUMP_L=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
canon() {           # canon OBJ -> one line per relocation: section, place, type
    "$OBJDUMP_L" -r "$1" > "$1.r" || return 1
    awk '/^RELOCATION RECORDS FOR/ { rs = $4; next }
         /^[0-9a-f]+ R_MIPS/ { print rs, $1, $2 }' "$1.r" | sort
}
if command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 &&
   "$MC" -triple=$MT -mcpu=mips32r2 /dev/null -o /dev/null \
       2>/dev/null
then
    cat > "$out/s.c" <<'CEOF'
extern void puts_(const char *s); extern void putn(long v);
int g = 5; static int sg[10]; int *pg = &g;
static const char *msg(int k) { return k ? "far" : "near"; }
int f(int x) { sg[x & 7] = x; return g + sg[x & 7] + *pg; }
int main(void) { puts_(msg(1)); putn(f(3)); puts_("\n==END==\n"); return 0; }
CEOF
    for f in boot io; do       # the harness, for the links (built above too)
        "$EMBCC" --target=$T -O1 -c "tests/harness/mips/$f.c" -o "$out/$f.o" ||
            { echo "the harness does not compile"; exit 1; }
    done
    "$EMBCC" --target=$T -c tests/golden/mips-exc/vector.S -o "$out/vector.o" ||
        { echo "-S: vector.S does not assemble"; exit 1; }
    for src in "$out/s.c -O2" "tests/exec/far-switch.c -O0" \
               "tests/golden/mips-exc/main.c -O2"; do
        set -- $src
        n=$(basename "$1" .c)
        extra=
        [ "$n" = main ] && extra="$out/vector.o"
        "$EMBCC" --target=$T "$2" -c "$1" -o "$out/$n-c.o" &&
        "$EMBCC" --target=$T "$2" -S "$1" -o "$out/$n.s" || {
            echo "-S: $1 does not compile"; exit 1; }
        "$MC" -triple=$MT -mcpu=mips32r2 -mattr=+soft-float \
            -filetype=obj "$out/$n.s" -o "$out/$n-s.o" 2> "$out/$n.mcerr" || {
            echo "-S: llvm-mc rejects the assembly for $1:"
            head -4 "$out/$n.mcerr"; exit 1; }
        for k in c s; do
            EMBCC_MIPS_HARNESS="$PWD/$out" sh tests/harness/mips/link.sh \
                "$out/$n-$k.elf" "$out/$n-$k.o" $extra || {
                echo "-S: $1's -$k object does not link"; exit 1; }
            "$OBJDUMP_L" -s -j .text -j .data -j .rodata -j .bss \
                "$out/$n-$k.elf" | tail -n +4 > "$out/$n-$k.img"
        done
        cmp -s "$out/$n-c.img" "$out/$n-s.img" || {
            echo "-S: $1 reassembles to a different program (-c, then -S):"
            diff "$out/$n-c.img" "$out/$n-s.img" | head -6; exit 1; }
        canon "$out/$n-c.o" > "$out/$n-c.rel" &&
        canon "$out/$n-s.o" > "$out/$n-s.rel" || {
            echo "-S: could not read $1's relocations"; exit 1; }
        [ -s "$out/$n-c.rel" ] || { echo "-S: $1 has no relocations to compare"; exit 1; }
        diff "$out/$n-c.rel" "$out/$n-s.rel" > "$out/$n.reldiff" || {
            echo "-S: $1's relocations differ (-c, then -S):"
            head -8 "$out/$n.reldiff"; exit 1; }
    done
    echo "-S reassembles with llvm-mc to -c's program and relocations, far jumps"
    echo "and asm blocks included"
else
    echo "SKIP the -S half: no llvm-mc with a MIPS target"
fi
