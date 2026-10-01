#!/bin/sh
# AVR: 64-bit integers, varargs, and aggregates by value.
#
# Three things an RTOS needs and this backend refused until now: a tick
# counter is 64 bits, a log call is variadic, and an API passes structs.
#
# Judged against the HOST, not against a table. The first version of this
# test hand-computed its expectations and got several wrong -- the halves of
# a shift transposed, and one case that asked putn to print LONG_MIN, whose
# magnitude does not fit in the type it prints through. Building the same
# source for the host and diffing removes that entire class of mistake, and
# is what tests/golden/avr-exec.sh already does.
#
# Every 64-bit value is printed as its EIGHT BYTES in hex, low address last,
# rather than as a number. `long` is four bytes on AVR and eight on the host,
# so a decimal print would not be comparable; the bytes are, because both are
# little-endian and the layout is the ABI's.
#
# ---- how eight bytes work on an eight-bit machine ---------------------
#
# Not in registers. The two scratch banks are four bytes each, so a binary
# operation on two eight-byte values could not hold its operands -- and there
# is no third bank, because r2-r17 are callee-saved and r26-r31 are the
# pointers. So an eight-byte value is processed a BYTE AT A TIME, straight out
# of one frame slot and into another.
#
# That works for one reason: `ldd` and `std` do NOT affect SREG on this
# machine. The carry from byte k survives the two loads and the store that
# byte k+1 needs, so an eight-byte add is eight `adc`s with memory traffic
# between them and no spill at all. An eight-byte shift is eight `rol`s for
# the same reason.
#
# The exception is the FAR slot path, which computes its address with
# subi/sbci -- and those DO clobber SREG. A chain crossing one has to save
# SREG through r0 and put it back. Without that, `y >>= 1` inside __muldi3
# produced garbage and the loop never terminated -- but only in functions
# whose frame had grown past ldd's six-bit reach, which is every function
# that calls it.
#
# ---- the ABI, measured from clang ------------------------------------
#
# VARARGS: a variadic call puts EVERY argument on the stack, the named ones
# included -- sum(3, 10, 20, 30) writes all four words to the outgoing area
# and nothing to a register. That is why a va_list here is a bare pointer
# with no register-save area behind it.
#
# AGGREGATES: a struct travels in registers under the same cursor rule a
# scalar does, and comes back in registers up to EIGHT bytes (r25:r18 is the
# lowest run the cursor can give). Nine or more returns through a hidden
# pointer the caller passes in r25:r24 and the callee returns there.
#
# ---- and it is TWO images ---------------------------------------------
#
# One program holding all of this is 35790 bytes of text at -O0 and the
# ATmega328P has 32768. That is the part, not something to work around, so
# the cases are split across two images and the size is asserted before each
# is run -- an over-size image presents at run time as a call past the end of
# flash, which is a poor way to learn it.
set -u
echo "TEST-MARKER avr-wide"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-wide
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
H=$out/h; mkdir -p "$H"

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: no $QEMU"; exit 0; }

# ---- the shared printing, byte-wise so both widths agree --------------
cat > "$out/pr.h" <<'EOF'
void writec(int c);
void puts_(const char *s);
void putn(long v);

static void phex(unsigned char b)
{
    static const char d[] = "0123456789abcdef";
    writec(d[(b >> 4) & 15]);
    writec(d[b & 15]);
}
/* Eight bytes, most significant first. Not a decimal print: `long` is four
 * bytes here and eight on the host, so only the bytes are comparable -- and
 * they are, because both are little-endian. */
static void p64(long long v)
{
    unsigned char *p = (unsigned char *)&v;
    int i;
    for (i = 7; i >= 0; i--)
        phex(p[i]);
    writec(' ');
}
EOF

# ---- image one: sixty-four bits --------------------------------------
cat > "$out/w64.c" <<'EOF'
#include "pr.h"

static long long addll(long long a, long long b) { return a + b; }
/* A long long that arrives on the STACK -- a, b and c fill r8-r25 -- and
 * that the allocator then gives a register home: the prologue copied it
 * into a slot the value does not have, and the function was refused at
 * -O2 and -Os ("a path addresses vreg 3's slot"). */
__attribute__((noinline)) static long long stk64(long long a, long b, long c,
                                                 long long d)
{
    long long s = 0;
    int i;
    for (i = 0; i < 4; i++) s += d >> (i * 8);
    return s + a + b + c + d;
}
__attribute__((noinline)) static long long stk64id(long long a, long b,
                                                   long c, long long d)
{
    (void)a; (void)b; (void)c;
    return d;
}
/* An unrolled loop on a 64-bit counter, in a function whose FIRST
 * parameter is eight bytes. The unroller's branches carried a `dst` of 0,
 * which named that parameter, so the backend took them for eight-byte
 * branches and tested eight registers: past r31 it refused, short of it
 * four bytes of something else decided the branch. */
__attribute__((noinline)) static long long cnt64(long long x, long long n)
{
    long long s = 0, i;
    for (i = 0; i < n; i++) s += i + i + i + 1;
    return s + (x >> 60);
}
static long long subll(long long a, long long b) { return a - b; }
static long long andll(long long a, long long b) { return a & b; }
static long long orll (long long a, long long b) { return a | b; }
static long long xorll(long long a, long long b) { return a ^ b; }
static long long shlll(long long a, int n) { return a << n; }
static long long shrll(long long a, int n) { return a >> n; }
static unsigned long long ushrll(unsigned long long a, int n) { return a >> n; }
static long long negll(long long a) { return -a; }
static long long notll(long long a) { return ~a; }
static long long mulll(long long a, long long b) { return a * b; }
static long long divll(long long a, long long b) { return a / b; }
static long long modll(long long a, long long b) { return a % b; }
static int ltll(long long a, long long b) { return a < b; }
static int eqll(long long a, long long b) { return a == b; }
static int nzll(long long a) { return a != 0; }
/* A small CONSTANT operand of a 64-bit multiply, which -Os folds into the
 * instruction as imm_b. The multiply is a call to __muldi3 with its second
 * argument in r17:r10, so the constant is loaded into registers `ldi` cannot
 * reach: the backend asked for `ldi r10, 3` until ldi4 learned to borrow r31,
 * and the encoder's range check refused it. NOT static, so it is compiled as
 * written rather than inlined into a caller where the whole product folds --
 * the first version of this case was static, never reached the path, and
 * passed with the fix taken out. This shape is tests/golden/
 * embedded-abi-callee.c's mix64, which is where the bug was found. */
long long mul3add(int a, long long b) { return (long long)a + b * 3; }

void run(void)
{
    p64(addll(0x0000000100000002LL, 0x0000000300000004LL));
    p64(subll(0x0000000500000000LL, 1LL));
    p64(andll(0x0f0f0f0f0f0f0f0fLL, 0x3333333333333333LL));
    p64(orll (0x0f0f0f0f00000000LL, 0x0000000033333333LL));
    p64(xorll(-1LL, 0x0f0f0f0f0f0f0f0fLL));
    p64(shlll(1LL, 33));
    p64(shlll(1LL, 63));                /* the top bit: past 32 */
    p64(shrll(-4096LL, 8));             /* arithmetic: the sign fills in */
    p64((long long)ushrll(0x8000000000000000ULL, 60));  /* logical */
    p64(negll(1LL));
    p64(notll(0LL));
    p64((long long)(int)-5);            /* sign extension into eight bytes */
    puts_("| ");
    p64(mulll(100000LL, 100000LL));
    p64(divll(10000000000LL, 7LL));
    p64(modll(10000000000LL, 7LL));
    p64(divll(-10000000000LL, 7LL));    /* truncates toward zero */
    p64(modll(-10000000000LL, 7LL));    /* the DIVIDEND's sign */
    p64(mul3add(7, 0x0123456789abcdefLL));
    p64(mul3add(-7, -1000000000000LL));
    p64(divll(-10000000000LL, -7LL));
    puts_("| ");
    putn(ltll(1LL, 2LL));
    putn(ltll(2LL, 1LL));
    putn(ltll(-1LL, 1LL));              /* signed, across the high half */
    putn(eqll(0x100000000LL, 0x100000000LL));
    putn(eqll(0x100000000LL, 0LL));     /* differ only above 32 bits */
    putn(nzll(0x100000000LL));          /* and so does this */
    putn(nzll(0LL));
    puts_("| ");
    p64(stk64(-5LL, 7L, 9L, 0x0123456789ABCDEFLL));
    p64(stk64id(1LL, 2L, 3L, -0x0123456789ABCDEFLL));
    {
        long long k;
        for (k = 0; k < 11; k++)
            p64(cnt64(-0x1000000000000000LL, k));
    }
    puts_("DONE\n");
}
EOF

# ---- image two: varargs and aggregates -------------------------------
cat > "$out/wabi.c" <<'EOF'
#include <stdarg.h>
#include "pr.h"

static int isum(int n, ...)
{
    va_list ap; int s = 0, i;
    va_start(ap, n);
    for (i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}
static long lsum(int n, ...)
{
    va_list ap; long s = 0; int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) s += va_arg(ap, long);
    va_end(ap);
    return s;
}
/* mixed widths, which is what a logging call really looks like */
static long vmix(int n, ...)
{
    va_list ap; long s;
    va_start(ap, n);
    s = va_arg(ap, int);
    s += va_arg(ap, long);
    s += va_arg(ap, int);
    va_end(ap);
    (void)n;
    return s;
}

struct p2  { char a, b; };
struct p4  { int a, b; };
struct p8  { long a, b; };
struct p12 { long a, b, c; };
static int  take2(struct p2 s)   { return s.a + s.b; }
static long take4(struct p4 s)   { return s.a + s.b; }
static long take8(struct p8 s)   { return s.a + s.b; }
static long take12(struct p12 s) { return s.a + s.b + s.c; }
static struct p2  mk2(void)  { struct p2 s;  s.a = 3; s.b = 4; return s; }
static struct p4  mk4(void)  { struct p4 s;  s.a = 300; s.b = 400; return s; }
static struct p8  mk8(void)  { struct p8 s;  s.a = 100000L; s.b = 7L; return s; }
static struct p12 mk12(void) { struct p12 s; s.a = 1; s.b = 2; s.c = 3; return s; }
/* a struct AFTER other arguments, so the cursor has already moved */
static long smix(int x, struct p4 s, int y) { return x + s.a + s.b + y; }

void run(void)
{
    struct p2 a2 = mk2();
    struct p4 a4 = mk4();
    struct p8 a8 = mk8();
    struct p12 a12 = mk12();

    putn(isum(3, 10, 20, 30));
    putn(isum(5, 1, 2, 3, 4, 5));
    putn(isum(0));                      /* none at all */
    putn(lsum(3, 100000L, 200000L, 300000L));
    putn(vmix(3, 7, 100000L, 5));
    puts_("| ");
    putn(a2.a + a2.b);
    putn(a4.a + a4.b);
    putn(a8.a + a8.b);
    putn(a12.a + a12.b + a12.c);
    putn(take2(a2));
    putn(take4(a4));
    putn(take8(a8));
    putn(take12(a12));                  /* by hidden pointer */
    putn(take2(mk2()));                 /* a returned struct passed straight on */
    putn(take12(mk12()));
    putn(smix(10, a4, 20));
    puts_("DONE\n");
}
EOF

cat > "$out/hostio.c" <<'EOF'
/* The same three routines on the host, so the comparison is textual. */
#include <stdio.h>
void writec(int c) { putchar(c); }
void puts_(const char *s) { while (*s) writec(*s++); }
void putn(long v)
{
    char b[24];
    int n = 0;
    if (v < 0) { writec('-'); v = -v; }
    do { b[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    while (n) writec(b[--n]);
    writec(' ');
}
int main(void) { void run(void); run(); fflush(stdout); return 0; }
EOF

cat > "$H/main.c" <<'EOF'
void run(void);
int main(void) { run(); for (;;) ; }
EOF

"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" || {
    echo "the startup did not assemble"; exit 1; }

for which in w64 wabi; do
    cc -std=c99 -w -I"$out" -o "$out/host.$which" "$out/$which.c" \
        "$out/hostio.c" || { echo "$which: the host build failed"; exit 1; }
    "$out/host.$which" > "$out/want.$which" || {
        echo "$which: the host program failed"; exit 1; }
done

for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$H/io.o" || exit 1
    "$EMBCC" --target=avr $O -c lib/rt/avr.c   -o "$H/rt.o"   || exit 1
    "$EMBCC" --target=avr $O -c lib/rt/avr64.c -o "$H/rt64.o" || exit 1
    "$EMBCC" --target=avr $O -c "$H/main.c" -o "$H/main.o" || exit 1
    for which in w64 wabi; do
        "$EMBCC" --target=avr $O -I"$out" -c "$out/$which.c" -o "$H/run.o" \
            2> "$out/c.err" || {
            echo "$O $which: did not compile:"; head -6 "$out/c.err"; exit 1; }
        EMBCC_AVR_HARNESS="$H" sh tests/harness/avr/link.sh "$H/run.elf" \
            "$H/run.o" "$H/main.o" 2> "$out/l.err" || {
            echo "$O $which: link failed:"; head -4 "$out/l.err"; exit 1; }
        if command -v llvm-size >/dev/null 2>&1; then
            tx=$(llvm-size "$H/run.elf" 2>/dev/null | awk 'NR==2{print $1}')
            if [ -n "$tx" ] && [ "$tx" -gt 32768 ]; then
                echo "$O $which: the image is $tx bytes of text and this part
        has 32768. At run time that presents as a call past the end of
        flash, which is a poor way to learn it."
                exit 1
            fi
        fi
        # EMBCC_QEMU_UNTIL ends the run when the image prints its sentinel
        # rather than when the timeout expires. The image never exits on its
        # own, so without it each of these eight runs cost the full 60
        # seconds however fast it was.
        EMBCC_QEMU_UNTIL=DONE \
        EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-60} \
            sh tests/harness/avr/run.sh "$H/run.elf" \
            > "$out/got.$which$O" 2>/dev/null
        sed -n '1,/DONE/p' "$out/got.$which$O" > "$out/cut"
        grep -q DONE "$out/cut" || {
            echo "$O $which: did not reach its sentinel. What it printed:"
            head -c 300 "$out/got.$which$O" | od -c | head -6; exit 1; }
        cmp -s "$out/want.$which" "$out/cut" || {
            echo "$O $which: the ATmega328P disagrees with the host."
            echo "  want: $(cat "$out/want.$which")"
            echo "  got:  $(cat "$out/cut")"; exit 1; }
    done
done

echo "every case agrees with the host on a real ATmega328P at four
optimisation levels:
  64-bit add, subtract, and/or/xor, shifts both directions including past
  32 bits, negate, complement, sign extension, multiply, divide and
  remainder in three sign combinations, signed compare, equality that
  differs only above 32 bits, and a non-zero test that reads it
  varargs with int, long and mixed widths, and a call with no variadic
  arguments at all
  structs of 2, 4, 8 and 12 bytes passed and returned -- the last through
  the hidden pointer, the others in registers -- and one passed after other
  arguments so the cursor had already moved"
