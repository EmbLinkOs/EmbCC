#!/bin/sh
# Naked functions and file-scope asm on the embedded targets, assembled by
# the target's own assembler (src/as/gas.c) and run.
#
# An RTOS's context switch is written as a naked function: FreeRTOS's
# Cortex-M ports save and restore a task in xPortPendSVHandler, a body of
# asm with no prologue, a `bl` into C, a literal naming pxCurrentTCB and
# an "i" operand. A naked function is assembled as a block that starts
# with its label, as gcc does; a file-scope block holds the target's own
# instructions. Each program below is run on QEMU at -O0 and -O2, and
# checks what can only be wrong silently:
#
#   - the "i" operand is written into the template (add_k);
#   - a call from the asm into C, and a global loaded through a literal
#     pool, are relocations against those symbols (via_c);
#   - a static naked function's label is a LOCAL symbol the C code's
#     call and function pointer go to -- not an undefined one that links
#     against nothing, and on Thumb with bit 0 set, or the call through
#     the pointer switches to the ARM state a Cortex-M does not have;
#   - a field naming an assembler-local label (.word .Ldata, RISC-V's
#     %pcrel_lo half) is placed against the section at the block's
#     offset, not the label's offset within the block alone;
#   - a block's literal pool is marked as data ($d) for disassemblers.
#
# And a naked function that holds anything else -- a C statement, an
# operand that is not a constant, an output -- is refused by name: it has
# no frame for those to live in.
set -u
echo "TEST-MARKER naked-asm"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/naked-asm
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
command -v llvm-objdump >/dev/null 2>&1 &&
command -v llvm-readelf >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }

# ---- Thumb ----------------------------------------------------------------
cat > "$out/thumb.c" <<'EOF'
void puts_(const char *s);
void putn(long v);
int counter = 5;
int twice(int x) { return 2 * x; }

int add_k(int a, int b);        /* as a header declares it: not naked */
int add_k(int a, int b) __attribute__((naked));
int add_k(int a, int b)
{
    __asm volatile("adds r0, r0, r1\n adds r0, %0\n bx lr\n" :: "i"(40));
}

/* naked on the prototype and not on the definition, as FreeRTOS writes */
static int via_c(int x) __attribute__((naked));
static int via_c(int x)
{
    __asm volatile(" push {r4, lr}\n"
                   " bl twice\n"
                   " ldr r4, =counter\n"
                   " ldr r4, [r4]\n"
                   " adds r0, r0, r4\n"
                   " pop {r4, pc}\n"
                   " .ltorg\n");
}

/* sum_table(n): the first n of three words, found through .Ladr */
__asm__(".text\n"
        ".global sum_table\n"
        ".type sum_table, %function\n"
        ".thumb_func\n"
        "sum_table:\n"
        "  ldr r1, .Ladr\n"
        "  movs r2, #0\n"
        "1: cbz r0, 2f\n"
        "  ldr r3, [r1], #4\n"
        "  add r2, r2, r3\n"
        "  subs r0, #1\n"
        "  b 1b\n"
        "2: mov r0, r2\n"
        "  bx lr\n"
        "  .p2align 2\n"
        ".Ladr: .word .Ldata\n"
        ".Ldata: .word 10, 20, 30\n");
int sum_table(int n);

int main(void)
{
    int (*volatile fa)(int, int) = add_k;
    int (*volatile fv)(int) = via_c;
    puts_("add_k "); putn(add_k(1, 2)); putn(fa(3, 4));
    puts_("\nvia_c "); putn(via_c(10)); putn(fv(1));
    puts_("\ntable "); putn(sum_table(3));
    puts_("\n==END==\n");
    return 0;
}
EOF
T=thumbv7m-none-eabi
"$EMBCC" --target=$T -O2 -Wall -Wextra -Werror -c "$out/thumb.c" \
    -o "$out/thumb.o" 2> "$out/thumb.err" ||
    { cat "$out/thumb.err"; fail "thumb.c did not compile warning-free"; }
llvm-objdump -dr --no-show-raw-insn "$out/thumb.o" > "$out/thumb.dis"
llvm-readelf -s "$out/thumb.o" > "$out/thumb.sym"
llvm-readelf -r "$out/thumb.o" > "$out/thumb.rel"
sed -n '/<add_k>:/,/^$/p' "$out/thumb.dis" | sed -n 2p |
    grep -q 'adds	r0, r0, r1' || { cat "$out/thumb.dis"; fail "add_k has a prologue"; }
grep -Eq '^ +[0-9]+: [0-9a-f]*[13579bdf] +6 FUNC +GLOBAL .* add_k$' "$out/thumb.sym" ||
    { cat "$out/thumb.sym"; fail "add_k: not a 6-byte global Thumb function"; }
grep -Eq '^ +[0-9]+: [0-9a-f]*[13579bdf] +[0-9]+ FUNC +LOCAL .* via_c$' "$out/thumb.sym" ||
    { cat "$out/thumb.sym"; fail "via_c: not a local Thumb function"; }
if grep -q 'UND via_c' "$out/thumb.sym"; then
    cat "$out/thumb.sym"; fail "via_c is also an undefined symbol"
fi
grep -q 'R_ARM_THM_CALL.*via_c' "$out/thumb.rel" ||
    { cat "$out/thumb.rel"; fail "main's call to via_c is not against its symbol"; }
grep -q 'R_ARM_THM_CALL.*twice' "$out/thumb.rel" &&
grep -q 'R_ARM_ABS32.*counter' "$out/thumb.rel" ||
    { cat "$out/thumb.rel"; fail "via_c's bl and literal are not relocations"; }
grep -q 'R_ARM_ABS32.*\.text' "$out/thumb.rel" ||
    { cat "$out/thumb.rel"; fail ".word .Ldata is not against .text"; }
# the literal pool of via_c is data: a $d at its address
pool=$(sed -n '/<via_c>:/,/^$/p' "$out/thumb.dis" | grep 'R_ARM_ABS32' |
       awk '{print $1}' | tr -d ':')
[ -n "$pool" ] && grep -Eq "^ +[0-9]+: 0*$pool +0 NOTYPE +LOCAL .* \\\$d" "$out/thumb.sym" ||
    { cat "$out/thumb.sym"; fail "via_c's literal pool at 0x$pool is not marked \$d"; }
echo "naked-asm thumb: no prologue, a local static label, relocations, \$d"

# ---- refused by name ------------------------------------------------------
refuse() {        # refuse TARGET 'body' 'message'
    printf 'int x;\nvoid g(int);\nint f(int a) __attribute__((naked));\nint f(int a) { %s }\n' \
        "$2" > "$out/r.c"
    if "$EMBCC" --target=$1 -c "$out/r.c" -o "$out/r.o" 2> "$out/r.err"; then
        fail "$1: accepted: $2"
    fi
    grep -q "$3" "$out/r.err" || { cat "$out/r.err"; fail "$1: '$2' refused without '$3'"; }
}
refuse $T 'x = a; __asm volatile("bx lr");' 'not an asm or a call with no arguments'
refuse $T '__asm volatile("mov r0, %0\n bx lr" :: "r"(a));' 'is not a constant'
refuse $T '__asm volatile("mov %0, r0\n bx lr" : "=r"(x));' 'has an output'
refuse riscv64-unknown-elf 'return a;' 'not an asm or a call with no arguments'
refuse avr 'g(a);' 'not an asm or a call with no arguments'
refuse x86_64-elf '__asm volatile("ret");' '__attribute__((naked)) is not supported'
refuse aarch64-elf '__asm volatile("ret");' '__attribute__((naked)) is not supported'
echo "naked-asm: a C statement, a register operand, an output, and naked off the embedded targets are refused"

# ---- RISC-V ---------------------------------------------------------------
cat > "$out/rv.c" <<'EOF'
void puts_(const char *s);
void putn(long v);
int counter = 5;
int twice(int x) { return 2 * x; }
#if __riscv_xlen == 64
#define SX "sd"
#define LX "ld"
#else
#define SX "sw"
#define LX "lw"
#endif

int add_k(int a, int b) __attribute__((naked));
int add_k(int a, int b)
{
    __asm volatile("add a0, a0, a1\n addi a0, a0, %0\n ret\n" :: "i"(40));
}

static int via_c(int x) __attribute__((naked));
static int via_c(int x)
{
    __asm volatile(" addi sp, sp, -16\n"
                   " " SX " ra, 8(sp)\n"
                   " call twice\n"
                   " la t0, counter\n"      /* %pcrel_hi and its lo half */
                   " lw t0, 0(t0)\n"
                   " add a0, a0, t0\n"
                   " " LX " ra, 8(sp)\n"
                   " addi sp, sp, 16\n"
                   " ret\n");
}

__asm__(".text\n"
        ".global sum_table\n"
        ".type sum_table, @function\n"
        "sum_table:\n"
        "  la a1, .Ldata\n"
        "  li a2, 0\n"
        "1: beqz a0, 2f\n"
        "  lw a3, 0(a1)\n"
        "  add a2, a2, a3\n"
        "  addi a1, a1, 4\n"
        "  addi a0, a0, -1\n"
        "  j 1b\n"
        "2: mv a0, a2\n"
        "  ret\n"
        "  .p2align 2\n"
        ".Ldata: .word 10, 20, 30\n");
int sum_table(int n);

int main(void)
{
    int (*volatile fa)(int, int) = add_k;
    int (*volatile fv)(int) = via_c;
    puts_("add_k "); putn(add_k(1, 2)); putn(fa(3, 4));
    puts_("\nvia_c "); putn(via_c(10)); putn(fv(1));
    puts_("\ntable "); putn(sum_table(3));
    puts_("\n==END==\n");
    return 0;
}
EOF

# ---- AVR: the FreeRTOS yield's shape, asm around calls into C --------------
cat > "$out/avr.c" <<'EOF'
void puts_(const char *s);
void putn(long v);
volatile unsigned char hits;
void bump(void) { hits++; }

int add_k(int a, int b) __attribute__((naked));
int add_k(int a, int b)
{
    __asm volatile("add r24, r22\n adc r25, r23\n adiw r24, %0\n ret\n" :: "i"(40));
}

static void bump_twice(void) __attribute__((naked));
static void bump_twice(void)
{
    __asm volatile("push r28");
    bump();
    bump();
    __asm volatile("pop r28\n ret");
}

int main(void)
{
    int (*volatile fa)(int, int) = add_k;
    void (*volatile fb)(void) = bump_twice;
    puts_("add_k "); putn(add_k(1, 2)); putn(fa(3, 4));
    bump_twice();
    fb();
    puts_("\nhits "); putn(hits);
    puts_("\n==END==\n");
    return 0;
}
EOF
for t in riscv64-unknown-elf avr; do
    "$EMBCC" --target=$t -O2 -Wall -Wextra -Werror -c "$out/$( [ $t = avr ] && echo avr || echo rv).c" \
        -o "$out/$t.o" 2> "$out/$t.err" || { cat "$out/$t.err"; fail "$t did not compile"; }
    llvm-readelf -s "$out/$t.o" > "$out/$t.sym"
    if grep -Eq 'UND (via_c|bump_twice)$' "$out/$t.sym"; then
        cat "$out/$t.sym"; fail "$t: a static naked function is an undefined symbol"
    fi
done
echo "naked-asm riscv64, avr: naked functions and blocks compile, static labels are local"

# ---- run ------------------------------------------------------------------
want_ab='add_k 43 47 
via_c 25 7 
table 60 
==END=='
want_avr='add_k 43 47 
hits 4 
==END=='
run_check() {     # run_check NAME WANT FILE
    tr -d '\r' < "$3" | sed -n '1,/==END==/p' > "$3.cut"
    [ "$(cat "$3.cut")" = "$2" ] || {
        echo "--- got"; cat "$3"; echo "--- want"; echo "$2"; fail "$1 printed the wrong thing"; }
}
H=$out/h; mkdir -p "$H"
if command -v "${EMBCC_QEMU_ARM:-qemu-system-arm}" >/dev/null 2>&1; then
    for f in boot io; do
        "$EMBCC" --target=$T -c tests/harness/thumb/$f.c -o "$H/$f.o" || exit 1
    done
    for O in -O0 -O2; do
        "$EMBCC" --target=$T $O -c "$out/thumb.c" -o "$out/t.o" || fail "thumb $O"
        EMBCC_THUMB_HARNESS="$H" sh tests/harness/thumb/link.sh "$out/t.elf" "$out/t.o" ||
            fail "thumb $O: link"
        sh tests/harness/thumb/run.sh "$out/t.elf" > "$out/t$O.txt" 2>&1
        run_check "thumb $O" "$want_ab" "$out/t$O.txt"
    done
    echo "naked-asm thumb: runs on the lm3s6965 at -O0 and -O2"
fi
if command -v "${EMBCC_QEMU_RISCV:-qemu-system-riscv64}" >/dev/null 2>&1; then
    R=riscv64-unknown-elf
    for f in boot io; do
        "$EMBCC" --target=$R -c tests/harness/riscv/$f.c -o "$H/rv$f.o" || exit 1
    done
    mkdir -p "$H/rv"; cp "$H/rvboot.o" "$H/rv/boot.o"; cp "$H/rvio.o" "$H/rv/io.o"
    for O in -O0 -O2; do
        "$EMBCC" --target=$R $O -c "$out/rv.c" -o "$out/r.o" || fail "riscv $O"
        EMBCC_RISCV_HARNESS="$H/rv" sh tests/harness/riscv/link.sh "$out/r.elf" "$out/r.o" ||
            fail "riscv $O: link"
        sh tests/harness/riscv/run.sh "$out/r.elf" > "$out/r$O.txt" 2>&1
        run_check "riscv64 $O" "$want_ab" "$out/r$O.txt"
    done
    echo "naked-asm riscv64: runs on virt at -O0 and -O2"
fi
if command -v "${EMBCC_QEMU_AVR:-qemu-system-avr}" >/dev/null 2>&1; then
    mkdir -p "$H/avr"
    "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/avr/boot.o" || exit 1
    for O in -O0 -O2; do
        "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$H/avr/io.o" || exit 1
        "$EMBCC" --target=avr $O -c lib/rt/avr.c -o "$H/avr/rt.o" || exit 1
        "$EMBCC" --target=avr $O -c "$out/avr.c" -o "$out/a.o" || fail "avr $O"
        EMBCC_AVR_HARNESS="$H/avr" sh tests/harness/avr/link.sh "$out/a.elf" "$out/a.o" ||
            fail "avr $O: link"
        EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$out/a.elf" \
            > "$out/a$O.txt" 2>&1
        run_check "avr $O" "$want_avr" "$out/a$O.txt"
    done
    echo "naked-asm avr: runs on the uno at -O0 and -O2"
fi
