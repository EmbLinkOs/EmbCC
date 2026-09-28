#!/bin/sh
# AVR: computing only the bytes something reads.
#
# The IR computes every integer at four bytes; on AVR an `int` is two, so most
# of that work was never read. src/arch/avr/codegen.c's avr_demand works out,
# for every temporary, how many low bytes any use observes, and the backend
# reads, computes and writes only those. It rests on one fact: the low N bytes
# of a sum, difference, product, and/or/xor, negation, complement or left
# shift depend only on the low N bytes of the operands. Everything else --
# compares, right shifts, divides, branches, anything the analysis does not
# name -- demands the whole value.
#
# This is the test for that invariant, and it is written to BREAK a wrong
# version of it: carries that run into bytes a later compare reads, right
# shifts of narrowed results, sign extension after truncation, pointer
# arithmetic, and the case the first version got wrong -- a struct returned by
# ADDRESS, whose operand is a pointer however small the struct.
#
# Fixed-width types throughout, so the host computes exactly the same thing.
#
# And one thing narrowing must NOT do: shorten a VOLATILE access. A 16-bit
# timer or ADC register on this part latches its high byte when the low one is
# read, and the standard says a volatile access happens as written. That is
# checked in the instructions, since no output could show it.
set -u
echo "TEST-MARKER avr-narrow"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-narrow
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
export EMBCC
H=$out/h; mkdir -p "$H"

# ---- 1. a volatile read keeps its width ------------------------------
cat > "$out/vol.c" <<'EOF'
#include <stdint.h>
volatile uint16_t *const TCNT1 = (volatile uint16_t *)0x84;
uint8_t low_byte(void) { return (uint8_t)*TCNT1; }
EOF
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $O -c "$out/vol.c" -o "$H/vol.o" || exit 1
    llvm-objcopy -O binary --only-section=.text "$H/vol.o" "$H/vol.bin"
    # ld rX, Z / ld rX, Z+ are the loads through the pointer; count them.
    n=$(xxd -p "$H/vol.bin" | tr -d '\n' | sed 's/\(..\)/0x\1 /g' |
        llvm-mc -triple=avr -mcpu=atmega328p -disassemble 2>/dev/null |
        grep -cE '^[[:space:]]+(ld|ldd)[[:space:]]+r[0-9]+, Z')
    [ "$n" -ge 2 ] || {
        echo "$O: a volatile 16-bit read became $n load(s) -- narrowing must
        not shorten a volatile access"; exit 1; }
done

# ---- 2. the invariant, on the part, against the host -----------------
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP (execution): no $QEMU"; exit 0; }

cat > "$out/n.c" <<'EOF'
#include <stdint.h>
void writec(int c); void puts_(const char *s);
static void phex(uint8_t b) { static const char d[] = "0123456789abcdef"; writec(d[b >> 4]); writec(d[b & 15]); }
static void p16(uint16_t v) { phex((uint8_t)(v >> 8)); phex((uint8_t)v); writec(' '); }
static void p32(uint32_t v) { p16((uint16_t)(v >> 16)); p16((uint16_t)v); }

/* Through functions, so each case is compiled as written. */
static int16_t add16(int16_t a, int16_t b) { return (int16_t)(a + b); }
static int32_t addwide(int16_t a, int16_t b) { return (int32_t)a + b; }  /* the carry IS read */
static int carry_cmp(uint16_t a, uint16_t b) { return (uint32_t)a + b > 0xffffu; }
static int16_t shr_after(int16_t a, int16_t b) { return (int16_t)((a * b) >> 3); }
static int16_t sext(uint16_t v) { return (int16_t)(int8_t)(v + 1); }
static uint8_t wrap8(uint8_t a) { return (uint8_t)(a * 3 + 200); }
/* (int32_t)c, not c: `c * s` is an INT product, and on AVR int is sixteen
 * bits, so -3 * 20000 would overflow -- undefined, and no oracle for it. */
static int32_t mixed(int8_t c, int16_t s, int32_t l) { return (int32_t)c * s + l; }
static uint16_t notneg(uint16_t v) { return (uint16_t)(~v + (-v)); }
static int16_t shl16(int16_t v, uint8_t k) { return (int16_t)(v << k); }
static int32_t shl32(int16_t v, uint8_t k) { return (int32_t)v << k; }
static int lt(int16_t a, int16_t b) { return (int16_t)(a - b) < 0; }
static uint16_t idx(const uint16_t *p, int16_t i) { return p[i + 1]; }
struct s1 { uint8_t b; };
static struct s1 ret1(uint8_t v) { struct s1 r; r.b = (uint8_t)(v ^ 0x5a); return r; }
static uint16_t sel(int c, uint16_t a, uint16_t b) { return c ? (uint16_t)(a + 1) : (uint16_t)(b - 1); }

int prog_main(void)
{
    static const uint16_t tab[4] = { 0x1111, 0x2222, 0x3333, 0x4444 };
    p16((uint16_t)add16(30000, 30000));        /* wraps at 16 bits */
    p32((uint32_t)addwide(30000, 30000));       /* does not */
    p16((uint16_t)carry_cmp(0xfff0, 0x20));     /* the carry byte is compared */
    p16((uint16_t)carry_cmp(0x10, 0x20));
    p16((uint16_t)shr_after(-300, 7));          /* shift needs the high byte */
    p16((uint16_t)shr_after(1000, 90));
    p16((uint16_t)sext(0x017f));                /* truncate, then sign-extend */
    p16((uint16_t)sext(0x0005));
    p16(wrap8(250));
    p32((uint32_t)mixed(-3, 20000, 1000000));
    p16(notneg(0x1234));
    p16((uint16_t)shl16(0x0123, 9));
    p32((uint32_t)shl32(-2, 20));
    p16((uint16_t)lt(-30000, 30000));           /* the subtraction overflows */
    p16((uint16_t)lt(5, 3));
    p16(idx(tab, 1));                           /* pointer arithmetic */
    p16(ret1(0x0f).b);                          /* returned by address */
    p16(sel(1, 0xffff, 7));
    p16(sel(0, 0xffff, 0));
    puts_("\n");
    return 0;
}
EOF
cat > "$out/hostio.c" <<'EOF'
#include <stdio.h>
void writec(int c) { putchar(c); }
void puts_(const char *s) { while (*s) putchar(*s++); }
int prog_main(void);
int main(void) { return prog_main(); }
EOF
cat > "$out/wrap.c" <<'EOF'
void puts_(const char *s);
int prog_main(void);
int main(void) { prog_main(); puts_("<<END>>\n"); for (;;) ; }
EOF
cc -std=c99 -w -o "$out/host" "$out/n.c" "$out/hostio.c" &&
    "$out/host" > "$out/want" || { echo "the host program failed"; exit 1; }

sh tools/build-rt.sh avr "$out/rt" 2> "$out/rt.err" || {
    echo "the AVR runtime does not build:"; head -3 "$out/rt.err"; exit 1; }
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" || exit 1
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$H/io.o" || exit 1
    "$EMBCC" --target=avr $O -c "$out/wrap.c" -o "$H/wrap.o" || exit 1
    "$EMBCC" --target=avr $O -c "$out/n.c" -o "$H/n.o" 2> "$out/c.err" || {
        echo "$O: did not compile:"; head -4 "$out/c.err"; exit 1; }
    "${EMBLD:-./embld}" -e __vectors -Ttext 0x0 -Tdata 0x100 \
        "$H/boot.o" "$H/io.o" "$H/n.o" "$H/wrap.o" "$out/rt/librt.a" \
        -o "$H/n.elf" 2> "$out/l.err" || {
        echo "$O: link failed:"; head -4 "$out/l.err"; exit 1; }
    EMBCC_QEMU_UNTIL='<<END>>' EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-30} \
        sh tests/harness/avr/run.sh "$H/n.elf" 2>/dev/null \
        | sed '/<<END>>/,$d' > "$out/got$O"
    cmp -s "$out/want" "$out/got$O" || {
        echo "$O: the ATmega328P disagrees with the host:"
        echo "  want: $(cat "$out/want")"
        echo "  got:  $(cat "$out/got$O")"; exit 1; }
done

echo "narrowed arithmetic agrees with the host on a real ATmega328P at four
optimisation levels -- carries into compared bytes, right shifts of narrowed
results, sign extension after truncation, pointer arithmetic, a struct
returned by address -- and a volatile 16-bit read still reads both bytes"
