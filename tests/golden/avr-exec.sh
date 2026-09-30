#!/bin/sh
# EmbCC's AVR code, RUN on an ATmega328P.
#
# The cases below are compiled twice from one source: once for AVR and run
# on qemu-system-avr's arduino-uno, once for the host and run natively.
# The two outputs must be identical. Judging against the host rather than
# against a table is what makes this test able to grow -- a case added
# below needs no expected value written down -- and it is the same
# arrangement the ARMv7-M execution tests use.
#
# What it is really checking is the part of an 8-bit backend that no
# amount of reading catches: every value here is a RUN of registers, and
# a carry chain that drops its last byte, an extension that fills with the
# wrong sign, or a comparison that tests the wrong SREG flag all produce
# code that runs and answers almost correctly. The first version of this
# backend had exactly that: enum avr_cond numbered its flags in mnemonic
# order, so `breq` tested carry, and `while (*s)` walked past its NUL.
#
# No float or long long: this backend refuses both by name (a soft-float
# runtime and a legalisation pass), and tests/golden/avr-refuse.sh is what
# checks that they are refused. Multiply, divide and remainder ARE here --
# they are calls into lib/rt/avr.c, built below at the same optimisation
# level as the program.
set -u
echo "TEST-MARKER avr-exec"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-exec
rm -rf "$out"; mkdir -p "$out"

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: no $QEMU"; exit 0; }
cat > "$out/prog.c" <<'EOF'
void writec(int c);
void puts_(const char *s);
void putn(long v);

/* ---- multi-register arithmetic, at every width ---------------------- */
static long g_l = 100000L;
static int  g_i = -3000;
static unsigned char g_uc = 200;
static signed char g_sc = -100;
static unsigned int g_ui = 60000u;

int  addi(int a, int b)        { return a + b; }
long subl(long a, long b)      { return a - b; }
long andl(long a, long b)      { return a & b; }
long orl (long a, long b)      { return a | b; }
long xorl(long a, long b)      { return a ^ b; }
long negl(long a)              { return -a; }
long notl(long a)              { return ~a; }

/* Six arguments: the cursor walks r25 down to r14, so this is the case
 * that proves the placement rule rather than the first two registers. */
int six(int a, int b, int c, int d, int e, int f)
{ return a + b + c + d + e + f; }

/* Ten: the tenth overflows onto the stack, which is the half of the ABI
 * a register-only test never reaches. The callee reads it at Y+frame+5. */
long ten(long a, int b, int c, int d, int e, int f, int g, int h, int i,
         int j)
{ return a + b + c + d + e + f + g + h + i + j; }

/* ---- multiply, divide and remainder: the runtime helpers -------------
 *
 * Every product below stays inside 32 bits, because the host's `long` is
 * eight bytes and AVR's is four -- an overflowing product is undefined and
 * the two would disagree about it for reasons that are not a bug.
 */
long mull(long a, long b)  { return a * b; }
long divl(long a, long b)  { return a / b; }
long modl(long a, long b)  { return a % b; }
unsigned long udivl(unsigned long a, unsigned long b) { return a / b; }
unsigned long umodl(unsigned long a, unsigned long b) { return a % b; }

/* ---- multiply by a constant: shifts and adds, in place ---------------
 *
 * No helper: the product is built in the destination from its top bit
 * down, and a negative constant negates it there -- com, neg and sbci
 * when the destination is from r16 up, which an int returned in r24 is.
 * A case whose low byte is nonzero is what tells sbci from subi. */
int  mulc10(int x)            { return x * 10; }
int  mulcm3(int x)            { return x * -3; }
long mulcm12(long x)          { return x * -12L; }
unsigned char mulc5b(unsigned char x) { return (unsigned char)(x * 5); }

/* ---- a value merged from two arms, then tested -----------------------
 *
 * The backend reads a value at the width its definitions give it (a char
 * compared as one byte), and a merge is that narrow only when BOTH arms
 * are, the same way, at the wider of their widths. Each case below
 * breaks one of those: mixed extensions, a narrower arm than the other,
 * and a negative constant in one arm. */
int mergesg(int c, signed char a, unsigned char b)   { int x = c ? a : b; return x < 0; }
int mergebrz(int c, unsigned char a, unsigned short b)
{ unsigned x = c ? a : b; return x ? 7 : 9; }
int mergeneg(int c, unsigned char b)                 { int x = c ? -1 : b; return x < 0; }

/* ---- shifts ---------------------------------------------------------- */
long shl(long v, int n)  { return v << n; }
long shr(long v, int n)  { return v >> n; }
unsigned long ushr(unsigned long v, int n) { return v >> n; }

/* ---- comparisons, both signednesses --------------------------------- */
static void cmps(long a, long b)
{
    writec('0' + (a == b)); writec('0' + (a != b));
    writec('0' + (a <  b)); writec('0' + (a <= b));
    writec('0' + (a >  b)); writec('0' + (a >= b));
    writec(' ');
}
static void ucmps(unsigned long a, unsigned long b)
{
    writec('0' + (a == b)); writec('0' + (a != b));
    writec('0' + (a <  b)); writec('0' + (a <= b));
    writec('0' + (a >  b)); writec('0' + (a >= b));
    writec(' ');
}

/* ---- pointers, arrays and aggregates in memory ----------------------- */
struct pt { int x, y; long tag; };
static struct pt tab[3];

static long sum_through_pointer(const int *p, int n)
{
    long s = 0;
    while (n--)
        s += *p++;
    return s;
}

/* ---- an indirect call: the Harvard case ----------------------------- */
/* A function pointer on AVR holds a WORD address, so `icall` and the _GS
 * relocations must agree. Getting it wrong calls twice as far into flash
 * and lands on a real instruction, so nothing faults. */
static int twice(int v)  { return v + v; }
static int thrice(int v) { return v + v + v; }
int (*volatile fp)(int);

void run(void)
{
    int i;
    int arr[5];
    struct pt a;

    /* widths and extension */
    putn(addi(1000, 234));
    putn(subl(100000L, 1L));
    putn(andl(0x0f0f0f0fL, 0x33333333L));
    putn(orl (0x0f0f0f0fL, 0x33333333L));
    putn(xorl(0x0f0f0f0fL, 0x33333333L));
    putn(negl(123456L));
    putn(notl(255L));
    puts_("| ");

    /* narrow types extend by their own signedness, not by convenience */
    putn(g_sc);
    putn(g_uc);
    putn(g_i);
    putn((long)g_ui);
    putn(g_l);
    putn((long)(signed char)200);
    putn((long)(unsigned char)-56);
    putn((long)(short)70000L);
    puts_("| ");

    /* argument placement, in registers and past them */
    putn(six(1, 2, 3, 4, 5, 6));
    putn(ten(1000L, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    puts_("| ");

    /* shifts: constant, byte-multiple, and variable */
    putn(shl(1L, 1));  putn(shl(1L, 8));  putn(shl(1L, 15));
    putn(shl(-1L, 3));
    putn(shr(-1024L, 3)); putn(shr(-1024L, 8));
    putn((long)ushr(0x80000000UL, 24));
    for (i = 0; i < 4; i++)
        putn(shl(3L, i * 5));
    puts_("| ");

    /* multiply, divide, remainder -- and the four corners that a
     * magnitude-then-sign divider gets wrong if it is written naively */
    putn(mull(1234L, 5678L));
    putn(mull(-1234L, 5678L));
    putn(mull(46340L, 46340L));         /* just under 2^31 */
    putn(mull(g_i, 7L));
    putn(divl(1000000L, 7L));
    putn(modl(1000000L, 7L));
    putn(divl(-100L, 7L));              /* truncates toward zero: -14 */
    putn(modl(-100L, 7L));              /* the DIVIDEND's sign: -2 */
    putn(divl(100L, -7L));
    putn(modl(100L, -7L));
    putn(divl(-100L, -7L));
    putn(modl(-100L, -7L));
    /* LONG_MIN: its magnitude does not fit in a signed long, which is why
     * the helper negates the UNSIGNED value. Written as a subtraction so
     * the constant itself is in range on both machines. */
    putn(divl(-2147483647L - 1L, 3L));
    putn(modl(-2147483647L - 1L, 3L));
    putn((long)udivl(4000000000UL, 123UL));   /* a dividend above 2^31 */
    putn((long)umodl(4000000000UL, 123UL));
    putn((long)udivl(4294967295UL, 65535UL));
    putn(mulc10(1234)); putn(mulc10(-77));
    putn(mulcm3(7)); putn(mulcm3(256)); putn(mulcm3(-85));
    putn(mulcm12(100000L)); putn(mulcm12(-3L));
    putn(mulc5b(200));
    putn(mergesg(1, -1, 0)); putn(mergesg(0, 0, 255));
    putn(mergebrz(0, 0, 256)); putn(mergebrz(1, 3, 0));
    putn(mergeneg(1, 0)); putn(mergeneg(0, 255));
    puts_("| ");

    /* comparisons */
    cmps(1L, 2L); cmps(2L, 1L); cmps(2L, 2L);
    cmps(-1L, 1L); cmps(1L, -1L);
    ucmps(1UL, 2UL); ucmps(0xffffffffUL, 1UL);
    puts_("| ");

    /* memory: arrays, structs, and the copies the compiler synthesises */
    for (i = 0; i < 5; i++)
        arr[i] = (i + 1) * 100;
    putn(sum_through_pointer(arr, 5));
    a.x = 7; a.y = 9; a.tag = 123456L;
    tab[1] = a;                     /* a struct copy: IR_MEMCPY */
    putn(tab[1].x + tab[1].y);
    putn(tab[1].tag);
    putn(tab[0].tag);               /* .bss, so zero */
    puts_("| ");

    /* control flow */
    {
        long s = 0;
        for (i = 1; i <= 10; i++) s += i;
        i = 0;
        do { s += 2; i++; } while (i < 3);
        while (s > 60) s -= 7;
        switch (i) {
        case 1:  s += 1000; break;
        case 3:  s += 3000; break;
        default: s += 9000; break;
        }
        putn(s);
    }
    puts_("| ");

    /* indirect calls */
    fp = twice;  putn(fp(21));
    fp = thrice; putn(fp(14));
    puts_("| ");

    /* a string walked a byte at a time: the loop whose zero test was
     * wrong the first time this backend ran */
    puts_("abcXYZ");
    writec(' ');
    {
        const char *p = "0123456789";
        long n = 0;
        while (*p) { n += *p - '0'; p++; }
        putn(n);
    }
    puts_("DONE\n");
}
EOF

cat > "$out/hostio.c" <<'EOF'
/* The same three output routines, on the host. putn's shape is copied
 * from the harness's so the two agree digit for digit -- including the
 * trailing space, which is what keeps the comparison textual. */
#include <stdio.h>
void writec(int c) { putchar(c); }
void puts_(const char *s) { while (*s) writec(*s++); }
/* Character for character the same routine as the harness's, powers of
 * ten and all -- a second implementation here could differ in its
 * leading zeroes or its sign and report a miscompile that was only a
 * disagreement between two printers. */
void putn(long v)
{
    /* No divide: this backend has none yet. But not repeated subtraction
     * of ten either -- that is O(v) and 50529027 took long enough for a
     * QEMU test to time out and look exactly like a miscompile. One
     * digit at a time against the powers of ten is at most nine
     * subtractions per digit, which is 90 for the widest value a
     * four-byte long holds. */
    static const long p10[10] = {
        1000000000L, 100000000L, 10000000L, 1000000L, 100000L,
        10000L, 1000L, 100L, 10L, 1L
    };
    int i, started = 0;
    if (v < 0) { writec('-'); v = -v; }
    for (i = 0; i < 10; i++) {
        int d = 0;
        while (v >= p10[i]) { v -= p10[i]; d++; }
        if (d || started || i == 9) { writec((int)('0' + d)); started = 1; }
    }
    writec(' ');
}
int main(void) { void run(void); run(); fflush(stdout); return 0; }
EOF

# ---- the host's answer -------------------------------------------------
# Built with the HOST compiler, so `long` is 8 bytes there and 4 on AVR.
# -DAVR_LONG is not used: every constant in the program fits in 32 bits
# and every intermediate is written to stay inside it, so the two agree.
cc -std=c99 -w -o "$out/host" "$out/prog.c" "$out/hostio.c" || {
    echo "the host build failed"; exit 1; }
"$out/host" > "$out/want" || { echo "the host program failed"; exit 1; }

# ---- the AVR answer ----------------------------------------------------
H=$out/h; mkdir -p "$H"
# EmbCC's OWN assembler, not llvm-mc: src/arch/avr/asm.c reads the startup
# and src/as/gas.c turns it into an object. That closes the last place this
# target needed another toolchain to produce a running image.
./embcc --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" \
    2> "$out/mc.err" || {
    echo "the AVR startup did not assemble:"; head -6 "$out/mc.err"; exit 1; }

for O in -O0 -O1 -O2 -Os; do
    ./embcc --target=avr $O -c tests/harness/avr/io.c -o "$H/io.o" \
        2> "$out/io.err" || {
        echo "$O: the harness io did not compile:"
        head -6 "$out/io.err"; exit 1; }
    # The compiler runtime: multiply, divide and remainder, which this
    # machine has no instructions for. Built at the same level as the
    # program, so a bug in the helpers shows up here too rather than only
    # at whatever level the library happened to be compiled at.
    ./embcc --target=avr $O -c lib/rt/avr.c -o "$H/rt.o" 2> "$out/rt.err" || {
        echo "$O: lib/rt/avr.c did not compile:"
        head -6 "$out/rt.err"; exit 1; }
    ./embcc --target=avr $O -c "$out/prog.c" -o "$H/prog.o" \
        2> "$out/prog.err" || {
        echo "$O: the program did not compile:"
        head -6 "$out/prog.err"; exit 1; }
    cat > "$H/main.c" <<'EOF'
void run(void);
int main(void) { run(); for (;;) ; }
EOF
    ./embcc --target=avr $O -c "$H/main.c" -o "$H/main.o" || {
        echo "$O: the entry did not compile"; exit 1; }
    EMBCC_AVR_HARNESS="$H" sh tests/harness/avr/link.sh "$H/p.elf" \
        "$H/prog.o" "$H/main.o" 2> "$out/ld.err" || {
        echo "$O: the link failed:"; head -4 "$out/ld.err"; exit 1; }
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-30} \
        sh tests/harness/avr/run.sh "$H/p.elf" > "$out/got.$O" 2>/dev/null
    # The image never exits, so the run is stopped by its timeout and
    # judged by the sentinel; everything after it is whatever the UART
    # was mid-way through when QEMU died.
    sed -n '1,/DONE/p' "$out/got.$O" > "$out/got.cut"
    grep -q DONE "$out/got.cut" || {
        echo "$O: the program did not reach its sentinel -- it hung, reset
        or faulted. What it did print:"
        head -c 400 "$out/got.$O" | od -c | head -8
        exit 1; }
    cmp -s "$out/want" "$out/got.cut" || {
        echo "$O: the ATmega328P disagrees with the host."
        echo "  want: $(cat "$out/want")"
        echo "  got:  $(cat "$out/got.cut")"
        exit 1; }
done

echo "every case agrees with the host on a real ATmega328P, at four
optimisation levels: multi-register arithmetic and its carry chains,
extension by each type's own signedness, arguments in registers and past
them onto the stack, constant and variable shifts, both signednesses of
every comparison, multiply/divide/remainder through lib/rt including
LONG_MIN and a dividend above 2^31, struct and array memory, and an
indirect call through a function pointer holding a WORD address"
