#!/bin/sh
# AVR inline __asm__: the operand classes, the byte modifiers, and what is
# refused.
#
# A kernel needs this on every target -- a critical section is `cli` and a
# context switch is a register save, and neither has a C spelling. On AVR
# there is a second reason: the constraint letters ARE the instruction set's
# restrictions. `ldi` cannot reach r15, `adiw` reaches four pairs, and only
# X/Y/Z address memory -- so "d", "w" and "e" are not conveniences, they are
# the difference between a template that assembles and one that does not.
#
# The template is assembled by src/arch/avr/asm.c, the same parser .S files
# go through and the same encoders the code generator uses, so an instruction
# written in an asm cannot be encoded differently from a compiled one.
set -u
echo "TEST-MARKER avr-inline-asm"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-inline-asm
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
H=$out/h; mkdir -p "$H"

# ---- what it refuses, and by name ------------------------------------
refuses() {                       # refuses <what> <phrase> <source>
    printf '%s\n' "$3" > "$out/r.c"
    if "$EMBCC" --target=avr -c "$out/r.c" -o "$out/r.o" 2> "$out/r.err"; then
        echo "$1: compiled, and it cannot work"; exit 1
    fi
    grep -q "$2" "$out/r.err" || {
        echo "$1: refused, but the message does not contain '$2':"
        head -3 "$out/r.err"; exit 1; }
}

# The frame pointer. A template that clobbers Y breaks the function around
# it -- every local is reached through Y -- and the asm itself would work,
# which is what makes it worth a diagnostic.
refuses "clobbering the frame pointer" "frame pointer" \
    'void f(void) { __asm__ volatile("clr r28" ::: "r28"); }'
refuses "naming the frame pointer" "frame pointer" \
    'void f(void) { __asm__ volatile("mov r28, r18"); }'
# A callee-saved register: EmbCC saves nothing around an asm.
refuses "clobbering a callee-saved register" "callee-saved" \
    'void f(void) { __asm__ volatile("clr r5" ::: "r5"); }'
# An unknown instruction reaches the assembler's refusal, not a guess.
refuses "an instruction that does not exist" "not an AVR instruction" \
    'void f(void) { __asm__ volatile("frobnicate r18, r19"); }'
# A constraint AVR does not have.
refuses "an x86 constraint" "not valid for AVR" \
    'void f(void) { int v = 1; __asm__ volatile("nop" : : "S"(v)); }'

# r1 is NOT refused: `mul` destroys it, so a template that multiplies must
# end with `clr r1`, and forbidding the name would forbid the only way to
# reach the machine's multiply from an asm. avr-gcc takes the same position.
printf 'unsigned int f(unsigned char a, unsigned char b)\n'\
'{ unsigned int r; __asm__ volatile("mul %%1, %%2\\n\\tmovw %%0, r0\\n\\tclr r1"\n'\
'  : "=r"(r) : "r"(a), "r"(b)); return r; }\n' > "$out/mulok.c"
"$EMBCC" --target=avr -c "$out/mulok.c" -o "$out/mulok.o" 2> "$out/m.err" || {
    echo "a template naming r1 was refused. mul destroys r1, so restoring it
    with 'clr r1' is the idiom -- refusing it forbids using mul at all:"
    head -4 "$out/m.err"; exit 1; }

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "the refusals hold (SKIP: no $QEMU to run the rest)"; exit 0; }

# ---- and what it does, on the part -----------------------------------
cat > "$out/run.c" <<'EOF'
void puts_(const char *s);
void putn(long v);

/* A critical section: what a kernel needs and what C cannot say. */
static unsigned char irq_save(void)
{
    unsigned char s;
    __asm__ volatile("in %0, 0x3f\n\tcli" : "=r"(s));
    return s;
}
static void irq_restore(unsigned char s)
{
    __asm__ volatile("out 0x3f, %0" : : "r"(s));
}

/* The machine's own multiply, which the code generator will not emit
 * because it destroys the zero register. The template puts it back. */
static unsigned int mul8(unsigned char a, unsigned char b)
{
    unsigned int r;
    __asm__ volatile("mul %1, %2\n\tmovw %0, r0\n\tclr r1"
                     : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/* %A and %B name the BYTES of a wider operand -- the normal way a template
 * handles a 16-bit value on an 8-bit machine. */
static unsigned int swap16(unsigned int v)
{
    unsigned int r;
    __asm__ volatile("mov %A0, %B1\n\tmov %B0, %A1" : "=r"(r) : "r"(v));
    return r;
}

/* "d" pins the operand to r16-r31, which is the only half `subi` reaches;
 * "i" folds the constant into the instruction. */
static unsigned char sub5(unsigned char v)
{
    unsigned char r;
    __asm__ volatile("mov %0, %1\n\tsubi %0, %2" : "=d"(r) : "r"(v), "i"(5));
    return r;
}

/* "e" pins a POINTER pair -- X, Y or Z, the only ones that address memory.
 * Y is the frame pointer, so this must land on X or Z. */
static unsigned char deref(const unsigned char *p)
{
    unsigned char r;
    __asm__ volatile("ld %0, %a1" : "=r"(r) : "e"(p));
    return r;
}

static const unsigned char tbl[4] = { 9, 8, 7, 6 };

int main(void)
{
    unsigned char s;
    long acc = 0;
    putn((long)mul8(13, 17));          /* 221 */
    putn((long)mul8(255, 255));        /* 65025 */
    putn((long)swap16(0x1234));        /* 0x3412 */
    putn((long)sub5(10));              /* 5 */
    putn((long)deref(tbl));            /* 9 */
    /* The critical section must not disturb anything around it. */
    s = irq_save();
    acc = 0x01020304L;
    irq_restore(s);
    putn(acc);
    /* And the multiply must still work after it: `clr r1` in the template
     * is what keeps every later instruction's zero register zero. */
    putn((long)mul8(3, 4));            /* 12 */
    putn(acc + 1);                     /* proves r1 is still zero */
    puts_("DONE\n");
    for (;;) ;
}
EOF
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" || exit 1
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$H/io.o" || exit 1
    "$EMBCC" --target=avr $O -c lib/rt/avr.c -o "$H/rt.o" || exit 1
    "$EMBCC" --target=avr $O -c "$out/run.c" -o "$H/run.o" 2> "$out/c.err" || {
        echo "$O: did not compile:"; head -6 "$out/c.err"; exit 1; }
    EMBCC_AVR_HARNESS="$H" sh tests/harness/avr/link.sh "$H/run.elf" \
        "$H/run.o" 2> "$out/l.err" || {
        echo "$O: link failed:"; head -4 "$out/l.err"; exit 1; }
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
        sh tests/harness/avr/run.sh "$H/run.elf" > "$out/got.$O" 2>/dev/null
    sed -n '1,/DONE/p' "$out/got.$O" > "$out/cut"
    want="221 65025 13330 5 9 16909060 12 16909061 DONE"
    got=$(tr -d '\n' < "$out/cut" | sed 's/  *$//')
    [ "$got" = "$want" ] || {
        echo "$O: the ATmega328P disagrees."
        echo "  want: $want"
        echo "  got:  $got"; exit 1; }
done

# ---- strings, alignments, data and %c, against llvm-mc ----------------
# .ascii/.asciz/.string, .p2align/.balign/.align with and without a fill
# and a maximum, the data directives -- .word is the MACHINE's word, two
# bytes, as GNU as has it -- and constants written in with %c0, as
# Linux's asm-offsets and EmbLinkRTOS's layout probes write them. All
# were "does not begin with an instruction". llvm-mc's bytes at every
# halfword phase in the section (tests/harness/asmdir.sh). `.align 3` is
# eight bytes, as GNU as reads it for AVR; llvm-mc reads a byte count
# there, so its copy says .p2align.
MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
if command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1
then
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
.ascii "a;b#c//d"
.word 0x1234, %c0
.short -2
.long 0x12345678
.quad -2
.p2align 1
EOF
    ASMDIR_MC_SED='s/^\.align /.p2align /'
    asmdir_referee "$out" avr "-triple=avr -mcpu=atmega328p" nop 2 \
        "$out/dirs.txt" || exit 1
    ASMDIR_MC_SED=
else
    echo "SKIP the directives against llvm-mc: llvm-mc/llvm-objcopy not found"
fi
refuses "%c of a register operand" "names a register operand" \
    'int f(int x){ __asm__ volatile(".byte %c0" : : "r"(x)); return x; }'

# ---- avr-gcc's __SREG__, __SP_L__ and __SP_H__ -------------------------
# avr-gcc writes `__SREG__ = 0x3f` (and SPH, SPL) at the top of the assembly
# it generates, so inline asm, a naked function's body and a file-scope asm
# block name them -- an RTOS port's context switch is written that way. Each
# must assemble to the same bytes as the I/O address written as a number.
io_src() {
    cat <<IOEOF
unsigned char lk(void) { unsigned char s;
    __asm__ __volatile__("in %0, $1\n\tcli" : "=r"(s) :: "memory"); return s; }
void ul(unsigned char k) { __asm__ __volatile__("out $1, %0" :: "r"(k) : "memory"); }
void sw(void) __attribute__((naked));
void sw(void) { __asm__ __volatile__("in r0, $1\n\tpush r0\n\tin r0, $2\n\tin r0, $3\n\t"
                                     "out $3, r0\n\tout $2, r0\n\tpop r0\n\tout $1, r0\n\tret"); }
__asm__(".globl q\nq:\n\tin r24, $1\n\tin r22, $2\n\tin r23, $3\n\tret\n");
IOEOF
}
io_src __SREG__ __SP_L__ __SP_H__ > "$out/ioname.c"
io_src 0x3f 0x3d 0x3e > "$out/ionum.c"
for v in ioname ionum; do
    "$EMBCC" --target=avr -Os -c "$out/$v.c" -o "$out/$v.o" 2> "$out/$v.err" ||
        { cat "$out/$v.err"; echo "the I/O register names do not assemble ($v)"; exit 1; }
    llvm-objdump -d "$out/$v.o" | grep -E '^ +[0-9a-f]+:' > "$out/$v.dis"
done
cmp -s "$out/ioname.dis" "$out/ionum.dis" ||
    { diff "$out/ionum.dis" "$out/ioname.dis"; echo "__SREG__/__SP_L__/__SP_H__ are not 0x3f/0x3d/0x3e"; exit 1; }
grep -q 'in	r0, 0x3f' "$out/ioname.dis" || { echo "no in r0, SREG in the naked body"; exit 1; }
echo "  __SREG__, __SP_L__, __SP_H__ are 0x3f, 0x3d, 0x3e in inline asm, a naked body and file-scope asm"

echo "inline asm works on a real ATmega328P at four optimisation levels: a
critical section through SREG, the machine's own mul with the clr r1 the
compiler will not emit for itself, %A/%B naming the bytes of a 16-bit
operand, \"d\" pinning an operand to the half subi reaches, \"i\" folding a
constant, and \"e\" pinning a pointer pair that is not the frame pointer
and the frame pointer, a callee-saved register, an unknown instruction and
a constraint from another machine are each refused by name"
