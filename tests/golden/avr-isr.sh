#!/bin/sh
# AVR interrupt handlers: __attribute__((signal)) and ((interrupt)).
#
# An RTOS cannot be written without these, and on AVR a handler is a
# genuinely different function from an ordinary one:
#
#   it returns with `reti`, not `ret`. `ret` leaves interrupts masked for the
#   rest of the program's life, which looks like a hang rather than a
#   miscompile -- and is why the attribute is refused on the targets that do
#   not implement it instead of being ignored;
#
#   it saves SREG, because the interrupt arrived between two instructions of
#   code that was mid-comparison and every flag belongs to that code;
#
#   it cannot assume r1 is zero. The machine's zero register is zero only by
#   convention and `mul` clobbers it, so an interrupt landing between a
#   `mul` and its `clr r1` sees a dirty one. The handler saves it, clears it
#   for its own body, and restores it.
#
# `signal` leaves interrupts disabled in the body; `interrupt` re-enables
# them on entry. One `sei` apart, and getting that backwards is a handler
# that can be re-entered when it must not be.
#
# ---- what is checked, and how -----------------------------------------
#
# Three ways, because no one of them covers it:
#
#   THE SHAPE, from the disassembly: the pushes, the SREG save, the `clr
#   r1`, the `reti`, and the presence or absence of the `sei`.
#
#   THE EXECUTION, by CALLING the handler from C and observing its effect on
#   the part. `reti` pops a return address exactly as `ret` does, so a
#   direct call exercises the prologue, the body and the epilogue, including
#   that everything it saved comes back.
#
#   THE HARDWARE DISPATCH, from QEMU's own instruction trace. A timer
#   interrupt is armed and the trace must show the vector's handler being
#   entered with its prologue. It is read from the trace rather than from
#   the program's output because QEMU's AVR timer model asserts the
#   interrupt continuously -- writing TIMSK1 = 0 and clearing TIFR1 in the
#   handler do not stop it -- so the handler is re-entered before the main
#   program can report anything. That is a limit of the emulated
#   peripheral, not of the generated code: the trace shows the prologue
#   saving every register and the epilogue reaching `reti` each time.
set -u
echo "TEST-MARKER avr-isr"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-isr
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
H=$out/h; mkdir -p "$H"

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
have_qemu=1
command -v "$QEMU" >/dev/null 2>&1 || have_qemu=0

# ---- the shape --------------------------------------------------------
cat > "$out/shape.c" <<'EOF'
volatile unsigned char n;
volatile unsigned long L;
void elsewhere(void);
__attribute__((signal))    void __vector_13(void) { n++; }
__attribute__((interrupt)) void __vector_14(void) { n++; }
__attribute__((signal))    void __vector_15(void) { elsewhere(); n++; }
__attribute__((signal))    void __vector_16(void) { L = L * 3 + (L >> 7); }
void plain(void) { n++; }
EOF
"$EMBCC" --target=avr -O1 -c "$out/shape.c" -o "$out/shape.o" \
    2> "$out/shape.err" || {
    echo "an ISR did not compile:"; head -5 "$out/shape.err"; exit 1; }

dis() {   # dis <symbol> -- the disassembly of one function
    llvm-objdump -d --triple=avr --mcpu=atmega328p --no-show-raw-insn \
        "$out/shape.o" 2>/dev/null |
        awk -v s="<$1>:" 'index($0,s){p=1;next} p&&/^$/{exit} p' |
        sed 's/^[[:space:]]*[0-9a-f]*:[[:space:]]*//;s/[[:space:]]\{1,\}/ /g'
}
command -v llvm-objdump >/dev/null 2>&1 || {
    echo "SKIP: no llvm-objdump"; exit 0; }

# written <dis>: the registers the body writes -- every instruction
# between the prologue's `clr r1` (and `sei`) and the epilogue's pops,
# read from llvm-objdump's text, not from the compiler's own decoder. A
# call writes what a callee may: r0, r18-r27, r30 and r31.
written() {
    awk '
    function add(r) { if (r ~ /^r[0-9]+$/) w[substr(r, 2) + 0] = 1 }
    function pair(r) { add(r); add("r" (substr(r, 2) + 1)) }
    function ptr(o) { if (o ~ /^-?[XYZ]\+?$/) { gsub(/[-+]/, "", o)
        pair(o == "X" ? "r26" : o == "Y" ? "r28" : "r30") } }
    { line[NR] = $0 }
    END {
        for (lo = 1; lo <= NR && line[lo] != "clr r1"; lo++) ;
        if (line[lo + 1] == "sei") lo++
        hi = NR
        while (hi > lo && line[hi - 1] ~ /^(pop r[0-9]+|out 0x3f, r0)$/) hi--
        for (k = lo + 1; k < hi; k++) {
            n = split(line[k], f, /[ ,]+/); op = f[1]
            if (op ~ /^(call|rcall|icall|eicall)$/) {
                add("r0"); for (r = 18; r <= 27; r++) add("r" r)
                add("r30"); add("r31"); continue }
            if (op ~ /^(movw|adiw|sbiw)$/) { pair(f[2]); continue }
            if (op ~ /^(mul|muls|mulsu|fmul|fmuls|fmulsu)$/) { pair("r0"); continue }
            if (op ~ /^(st|std|sts)$/) { ptr(f[2]); continue }
            if (op == "ld") { add(f[2]); ptr(f[3]); continue }
            if (op ~ /^(out|push|cp|cpc|cpi|cpse|sbrc|sbrs|sbic|sbis|cbi|sbi|bst|ret|reti|nop|sleep|wdr|sec|clc|sei|cli|bset|bclr)$/ ||
                op ~ /^(rjmp|jmp|ijmp|br)/) continue
            add(f[2])
        }
        for (r in w) print r
    }' "$1" | sort -n
}

for v in __vector_13 __vector_14 __vector_15 __vector_16; do
    dis "$v" > "$out/$v.dis"
    head -1 "$out/$v.dis" | grep -q '^push r0$' || {
        echo "$v does not begin by saving r0 -- SREG is read through it:"
        head -3 "$out/$v.dis"; exit 1; }
    grep -q '^in r0, 0x3f$' "$out/$v.dis" || {
        echo "$v does not save SREG; the interrupted code's flags are lost"
        exit 1; }
    grep -q '^push r1$' "$out/$v.dis" || {
        echo "$v does not save r1. The zero register is zero only by
        convention and mul clobbers it, so a handler that lands between a
        mul and its clr r1 must put back what it found"; exit 1; }
    grep -q '^clr r1$' "$out/$v.dis" || {
        echo "$v does not clear r1 for its own body, which every lowering
        and everything it calls reads as zero"; exit 1; }
    tail -1 "$out/$v.dis" | grep -q '^reti$' || {
        echo "$v does not end with reti. `ret` would leave interrupts
        masked for the rest of the program's life:"
        tail -3 "$out/$v.dis"; exit 1; }
    # Every register the body writes is saved, and given back in the
    # opposite order: the interrupted code finds what it left.
    for r in $(written "$out/$v.dis"); do
        grep -q "^push r$r\$" "$out/$v.dis" || {
            echo "$v writes r$r and does not save it:"; cat "$out/$v.dis"
            exit 1; }
    done
    pu=$(sed -n 's/^push r//p' "$out/$v.dis" | tr '\n' ' ')
    po=$(sed -n 's/^pop r//p' "$out/$v.dis" | sed '1!G;h;$!d' | tr '\n' ' ')
    [ "$pu" = "$po" ] || {
        echo "$v pushes '$pu' and pops (reversed) '$po'"; exit 1; }
done
# ...and only those: a handler that adds one to a byte saves r0, SREG, r1
# and the one register the lds, the add and the sts take, as avr-gcc's
# does -- not the seventeen every handler once saved whatever it did
np=$(grep -c '^push ' "$out/__vector_13.dis")
[ "$np" -le 4 ] || {
    echo "__vector_13 (n++) pushes $np registers:"; cat "$out/__vector_13.dis"
    exit 1; }
# a handler that calls saves all that the callee may write
for r in 18 19 20 21 22 23 24 25 26 27 30 31; do
    grep -q "^push r$r\$" "$out/__vector_15.dis" || {
        echo "__vector_15 calls and does not save r$r"; exit 1; }
done

# `signal` must NOT re-enable interrupts; `interrupt` must.
grep -q '^sei$' "$out/__vector_13.dis" && {
    echo "a ((signal)) handler re-enabled interrupts. That is ((interrupt))'s
    behaviour, and a handler that can be re-entered when the source said it
    must not be is the hardest kind of RTOS bug to find"; exit 1; }
grep -q '^sei$' "$out/__vector_14.dis" || {
    echo "an ((interrupt)) handler did not re-enable interrupts, which is
    the one thing that distinguishes it from ((signal))"; exit 1; }

# An ordinary function must not have paid for any of it.
dis plain > "$out/plain.dis"
grep -q '^reti$' "$out/plain.dis" && {
    echo "an ordinary function ends with reti"; exit 1; }
# NOT by looking for `in r0, 0x3f`: every function with a frame has that,
# because writing the stack pointer through two I/O registers has to be
# bracketed by cli or an interrupt lands on a half-updated SP. What only a
# handler does is PUSH r0 -- and push it first, before anything else.
head -1 "$out/plain.dis" | grep -q '^push r0$' && {
    echo "an ordinary function begins by saving r0, which only a handler
    needs to do"; exit 1; }
grep -q '^push r1$' "$out/plain.dis" && {
    echo "an ordinary function saves the zero register"; exit 1; }

# ---- the refusals -----------------------------------------------------
# The hardware calls a handler, so there is no caller to agree with.
printf '__attribute__((signal)) void f(int a) { (void)a; }\n' > "$out/r1.c"
"$EMBCC" --target=avr -c "$out/r1.c" -o "$out/r1.o" 2> "$out/r1.err" && {
    echo "a handler with a parameter compiled; it would read the argument
    out of whatever the interrupted code left in that register"; exit 1; }
grep -q "parameters" "$out/r1.err" || {
    echo "refused, but not by name:"; head -3 "$out/r1.err"; exit 1; }

printf '__attribute__((signal)) int f(void) { return 1; }\n' > "$out/r2.c"
"$EMBCC" --target=avr -c "$out/r2.c" -o "$out/r2.o" 2> "$out/r2.err" && {
    echo "a handler returning a value compiled; reti goes back to the
    interrupted instruction and nothing is there to receive it"; exit 1; }

# And the attribute is refused where it is not implemented, rather than
# ignored -- a handler returning with `ret` masks interrupts forever.
printf '__attribute__((signal)) void f(void) { }\n' > "$out/r3.c"
"$EMBCC" --target=x86_64-elf -c "$out/r3.c" -o "$out/r3.o" \
    2> "$out/r3.err" && {
    echo "((signal)) was accepted on x86-64, where nothing implements it"
    exit 1; }

[ "$have_qemu" = 1 ] || {
    echo "the shape and the refusals hold (SKIP: no $QEMU for the rest)"
    exit 0; }

# ---- the execution, by calling the handler ----------------------------
cat > "$out/call.c" <<'EOF'
void puts_(const char *s);
void putn(long v);

volatile unsigned int count;
volatile long guard;

__attribute__((signal)) void __vector_13(void)
{
    count = count + 7;
}

int main(void)
{
    /* Values in every register the handler saves, checked afterwards: a
     * handler that restores one of them wrongly corrupts the code it
     * interrupted, and nothing else here would notice. */
    long a = 0x01020304L, b = 0x05060708L;
    guard = 0x0a0b0c0dL;
    __vector_13();
    __vector_13();
    putn((long)count);          /* 14 */
    putn(a + b);                /* 0x06080a0c */
    putn(guard);                /* 168496141 */
    puts_("DONE\n");
    for (;;) ;
}
EOF
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" || {
    echo "the startup did not assemble"; exit 1; }
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$H/io.o" || exit 1
    "$EMBCC" --target=avr $O -c lib/rt/avr.c -o "$H/rt.o" || exit 1
    "$EMBCC" --target=avr $O -c "$out/call.c" -o "$H/call.o" \
        2> "$out/c.err" || {
        echo "$O: the calling test did not compile:"
        head -5 "$out/c.err"; exit 1; }
    EMBCC_AVR_HARNESS="$H" sh tests/harness/avr/link.sh "$H/call.elf" \
        "$H/call.o" 2> "$out/l.err" || {
        echo "$O: link failed:"; head -4 "$out/l.err"; exit 1; }
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
        sh tests/harness/avr/run.sh "$H/call.elf" > "$out/got.$O" 2>/dev/null
    sed -n '1,/DONE/p' "$out/got.$O" > "$out/cut"
    grep -q DONE "$out/cut" || {
        echo "$O: the handler did not return to its caller. reti pops a
        return address exactly as ret does, so a direct call must come
        back. What it printed:"
        head -c 200 "$out/got.$O" | od -c | head -4; exit 1; }
    want="14 101190156 168496141 DONE"
    got=$(tr -d '\n' < "$out/cut" | sed 's/  *$//')
    [ "$got" = "$want" ] || {
        echo "$O: a handler corrupted its caller."
        echo "  want: $want"
        echo "  got:  $got"; exit 1; }
done

# ---- the hardware dispatch, from QEMU's trace ------------------------
cat > "$out/hw.c" <<'EOF'
void puts_(const char *s);
#define TCCR1B (*(volatile unsigned char *)0x81u)
#define TIMSK1 (*(volatile unsigned char *)0x6Fu)
#define SREG   (*(volatile unsigned char *)0x5Fu)
volatile unsigned int count;
__attribute__((signal)) void __vector_13(void) { count = count + 1; }
int main(void)
{
    puts_("ARMED ");
    TCCR1B = 1;                 /* clk/1 */
    TIMSK1 = 1;                 /* TOIE1 */
    SREG = SREG | 0x80;         /* sei */
    for (;;) ;
}
EOF
"$EMBCC" --target=avr -O1 -c "$out/hw.c" -o "$H/hw.o" || exit 1
EMBCC_AVR_HARNESS="$H" sh tests/harness/avr/link.sh "$H/hw.elf" "$H/hw.o" \
    || exit 1
# Vector 13 is TIMER1_OVF on an ATmega328P, and its slot is 13*4 bytes into
# the table. The linked image must jump from there to our handler.
slot=$(llvm-objdump -d --triple=avr --mcpu=atmega328p --no-show-raw-insn \
    "$H/hw.elf" 2>/dev/null |
    sed 's/^[[:space:]]*//;s/[[:space:]]\{1,\}/ /g' |
    sed -n 's/^34: jmp \(0x[0-9a-f]*\)$/\1/p')
addr=$(llvm-nm "$H/hw.elf" | sed -n 's/^0*\([0-9a-f]*\) T __vector_13$/0x\1/p')
[ -n "$slot" ] && [ "$slot" = "$addr" ] || {
    echo "the TIMER1_OVF vector slot holds '$slot' and __vector_13 is at
    '$addr' -- the table does not reach the handler"; exit 1; }

timeout 8 "$QEMU" -M uno -nographic -bios "$H/hw.elf" \
    -d in_asm -D "$out/trace.log" >/dev/null 2>&1
[ -s "$out/trace.log" ] || {
    echo "QEMU produced no trace; cannot confirm hardware dispatch"; exit 1; }
grep -q "IN: __vector_13" "$out/trace.log" || {
    echo "the hardware never dispatched to the handler. The vector slot is
    right, so either the interrupt was not enabled or the table is not at
    address 0"; exit 1; }
# and the prologue really ran, in order: every block QEMU translated of
# the handler (QEMU 8.2 ends one at each 256-byte page, so a prologue can
# straddle two)
awk '/^IN:/{keep = /IN: __vector_13$/} keep' "$out/trace.log" > "$out/first"
for want in "PUSH      r0" "IN        r0, \$63" "PUSH      r1" "EOR       r1, r1"; do
    grep -q "$want" "$out/first" || {
        echo "the handler was entered but its prologue did not run '$want':"
        head -8 "$out/first"; exit 1; }
done
grep -q "RETI" "$out/trace.log" || {
    echo "no reti executed; the handler never returned"; exit 1; }

echo "a ((signal)) handler saves r0, SREG, r1 and every register this
backend touches, clears r1 for its body, and returns with reti; an
((interrupt)) one also re-enables interrupts and a ((signal)) one does not;
an ordinary function pays for none of it
a handler called directly returns with its caller's registers intact, at
four optimisation levels on a real ATmega328P
and the hardware itself dispatched through the vector table into the
handler, whose prologue and reti QEMU's own trace shows executing"
