#!/bin/sh
# avr-gcc's __builtin_avr_* (src/sema/sema.c avr_builtin).
#
#  1. __builtin_avr_delay_cycles(n) takes exactly n cycles: its code, for n
#     from 0 to past each loop's reach, is counted with an ATmega's (AVR5)
#     timings -- ldi, dec, subi, sbci and nop 1, sbiw and rjmp 2, brne 2
#     taken and 1 not -- and must come to n. A count that is not a constant
#     is refused, as avr-gcc refuses it.
#  2. On qemu-system-avr's uno: swap, fmul, fmuls, fmulsu and insert_bits
#     against the C of what each means, at -O0 to -Os; sei, cli, wdr,
#     nop, nops and a delay run and return (sleep is compiled, not run:
#     QEMU halts at one whatever SMCR's SE says, where an ATmega goes on).
#  3. They are AVR's alone: on another target __has_builtin says no.
set -u
echo "TEST-MARKER avr-builtins"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/avr-builtins
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- 1. delay_cycles counted --------------------------------------------
# cycles OBJ: the cycles of f's straight line and loop, from the ldi's
# values (the count) and the loop body between the last ldi and the brne
# (--mcpu: without one, LLVM 18 decodes no sbiw)
cycles() {
    llvm-objdump -d --mcpu=atmega328p --no-show-raw-insn "$1" | sed -n '/<f>:/,$p' |
        sed -n 's/^ *[0-9a-f]*:[[:space:]]*//p' | awk '
        $1 == "ldi"  { split($0, a, ","); v = a[2]; sub(/^ */, "", v);
                       k += strtonum(v) * mult; mult *= 256; pre++; next }
        $1 == "dec" || $1 == "subi" || $1 == "sbci" { body += 1; next }
        $1 == "sbiw" { body += 2; next }
        $1 == "brne" { loop = 1; next }
        $1 == "rjmp" { tail += 2; next }
        $1 == "nop"  { tail += 1; next }
        $1 == "ret"  { next }
        { bad = $1 }
        BEGIN { mult = 1 }
        END { if (bad != "") { print "unexpected " bad; exit }
              print pre + (loop ? k * (body + 2) - 1 : 0) + tail }'
}
# (an awk without strtonum: the immediates are written in hex, 0x..)
awk 'BEGIN { exit strtonum("0x10") == 16 ? 0 : 1 }' 2>/dev/null || {
    cycles() {
        llvm-objdump -d --mcpu=atmega328p --no-show-raw-insn "$1" | sed -n '/<f>:/,$p' |
            sed -n 's/^ *[0-9a-f]*:[[:space:]]*//p' > "$out/c.dis"
        pre=0; body=0; loop=0; tail=0; k=0; mult=1
        while read op a b; do
            case $op in
                ldi) k=$((k + ${b} * mult)); mult=$((mult * 256)); pre=$((pre + 1)) ;;
                dec|subi|sbci) body=$((body + 1)) ;;
                sbiw) body=$((body + 2)) ;;
                brne) loop=1 ;;
                rjmp) tail=$((tail + 2)) ;;
                nop) tail=$((tail + 1)) ;;
                ret) ;;
                *) echo "unexpected $op"; return ;;
            esac
        done < "$out/c.dis"
        [ $loop = 1 ] && echo $((pre + k * (body + 2) - 1 + tail)) || echo $((pre + tail))
    }
}
n=0
for c in 0 1 2 5 6 7 8 100 765 766 767 768 1000 4567 262141 262142 262143 \
         1000000 16000000 83886077 83886078 83886079 100000000 4294967295; do
    printf 'void f(void) { __builtin_avr_delay_cycles(%sUL); }\n' "$c" > "$out/d.c"
    "$EMBCC" --target=avr -O2 -c "$out/d.c" -o "$out/d.o" || fail "delay_cycles($c) does not compile"
    got=$(cycles "$out/d.o")
    [ "$got" = "$c" ] || {
        llvm-objdump -d --mcpu=atmega328p "$out/d.o"; fail "delay_cycles($c) takes $got cycles"; }
    n=$((n + 1))
done
printf 'void f(unsigned long n) { __builtin_avr_delay_cycles(n); }\n' > "$out/bad.c"
if "$EMBCC" --target=avr -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
    fail "a delay_cycles of a variable was accepted"
fi
grep -q "takes a constant number of cycles, as in avr-gcc" "$out/bad.err" || {
    cat "$out/bad.err"; fail "a variable delay was refused, but not by name"; }
echo "avr-builtins: delay_cycles(n) takes exactly n cycles for $n counts from 0 to 4294967295; a variable count is refused by name"

# ---- 3. AVR's alone --------------------------------------------------------
printf '#if __has_builtin(__builtin_avr_swap)\n#error yes\n#endif\nint x;\n' > "$out/no.c"
"$EMBCC" --target=riscv32-unknown-elf -c "$out/no.c" -o /dev/null ||
    fail "riscv32's __has_builtin says it has __builtin_avr_swap"

# ---- 2. on the uno ----------------------------------------------------------
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP the run: $QEMU absent"; exit 0; }
EMBCC_AVR_HARNESS=$PWD/$out; export EMBCC_AVR_HARNESS
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/io.o" &&
"$EMBCC" --target=avr -Os -c lib/rt/avr.c -o "$out/rt-avr.o" || fail "the harness"
cat > "$out/p.c" <<'EOF'
void puts_(const char *s);
void putn(long v);
static volatile unsigned char seeds[] = { 0, 1, 0x0f, 0x80, 0x81, 0x7f, 0xa5, 0xff };
static unsigned char ref_ins(unsigned long map, unsigned char b, unsigned char v)
{
    unsigned char r = 0;
    for (int i = 0; i < 8; i++) {
        unsigned s = map >> (4 * i) & 0xf;
        r |= (unsigned char)(((s == 0xf ? v >> i : b >> s) & 1) << i);
    }
    return r;
}
int main(void)
{
    int bad = 0;
    for (unsigned i = 0; i < sizeof seeds; i++)
        for (unsigned j = 0; j < sizeof seeds; j++) {
            unsigned char a = seeds[i], b = seeds[j];
            bad |= __builtin_avr_swap(a) != (unsigned char)(a << 4 | a >> 4);
            bad |= __builtin_avr_fmul(a, b) != (unsigned)((unsigned)a * b << 1);
            bad |= __builtin_avr_fmuls(a, b) !=
                   (int)(unsigned)((int)(signed char)a * (signed char)b << 1);
            bad |= __builtin_avr_fmulsu(a, b) !=
                   (int)(unsigned)((int)(signed char)a * b << 1);
            bad |= __builtin_avr_insert_bits(0x76543210UL, a, b) != a;
            bad |= __builtin_avr_insert_bits(0x01234567UL, a, b) !=
                   ref_ins(0x01234567UL, a, b);
            bad |= __builtin_avr_insert_bits(0xffff0123UL, a, b) !=
                   ref_ins(0xffff0123UL, a, b);
            bad |= __builtin_avr_insert_bits(0xf7f5f3f1UL, a, b) !=
                   ref_ins(0xf7f5f3f1UL, a, b);
        }
    __builtin_avr_cli();
    __builtin_avr_sei();
    __builtin_avr_cli();
    __builtin_avr_wdr();
    if (seeds[0])               /* (never: QEMU halts at a sleep, SE or no) */
        __builtin_avr_sleep();
    __builtin_avr_nop();
    __builtin_avr_nops(5);
    __builtin_avr_delay_cycles(1000);
    __builtin_avr_delay_cycles(300000);
    puts_(bad ? "builtins BAD\n==END==\n" : "builtins ok\n==END==\n");
    for (;;)
        ;
}
EOF
k=0
for o in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $o -c "$out/p.c" -o "$out/p$o.o" 2> "$out/p.err" || {
        cat "$out/p.err"; fail "$o: does not compile"; }
    sh tests/harness/avr/link.sh "$out/p$o.elf" "$out/p$o.o" > "$out/p.ln" 2>&1 || {
        cat "$out/p.ln"; fail "$o: does not link"; }
    got=$(EMBCC_QEMU_UNTIL=END sh tests/harness/avr/run.sh "$out/p$o.elf" 2>/dev/null | grep builtins)
    [ "$got" = "builtins ok" ] || fail "$o on the uno: ${got:-no output}"
    k=$((k + 1))
done
echo "avr-builtins: swap, fmul, fmuls, fmulsu and insert_bits agree with their C, and sei/cli/wdr/nop/nops/delay run, at $k levels on the ATmega328P"
