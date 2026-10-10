#!/bin/sh
# EmbCC's avr-libc-compatible headers (include/avr): <avr/io.h> and the rest.
#  1. Each header compiles alone under -Wall -Wextra -Werror at -O0 and -Os
#     for every -mmcu= part, and <avr/io.h> in a .S file, where a register
#     is its number; none is found for another target.
#  2. Every register's address and every bit's number agree with a table
#     taken from the datasheet's register summary
#     (tests/golden/avr-io/m328p-regs.txt), and every register the header
#     names is in that table; BODS/BODSE exist on the picoPower parts only
#     and EEAR9 on the 1 KB-EEPROM parts only.
#  3. Each vector name is __vector_N for the datasheet's N
#     (tests/golden/avr-io/m328p-vectors.txt), and each part's memories are
#     its datasheet's.
#  4. A program using the interface -- ISR() and its options, PROGMEM,
#     PSTR, pgm_read_*, wdt_*, sleep_*, _delay_*, bit_is_* -- compiles
#     warning-free; the watchdog's two timed stores are adjacent, and the
#     delay loop branches back to its own sbiw.
# What these headers do on the board is avr-libc-run.sh's (pgm_read_* and
# the _P functions) and avr-crt.sh's (interrupts through the vector table).
set -u
echo "TEST-MARKER avr-io"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/avr-io
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-io
PARTS="atmega328p atmega328 atmega168p atmega168"
HDRS="avr/io.h avr/sfr_defs.h avr/common.h avr/interrupt.h avr/pgmspace.h
avr/wdt.h avr/sleep.h avr/eeprom.h avr/cpufunc.h util/delay.h util/delay_basic.h"

# 1. each header alone
n=0
for h in $HDRS; do
    f=$out/$(echo "$h" | tr / _).c
    { echo '#define F_CPU 16000000UL'; echo "#include <$h>"; echo 'int emb_io_unit;'; } > "$f"
    for p in $PARTS; do
        for O in -O0 -Os; do
            "$EMBCC" --target=avr -mmcu=$p $O -Wall -Wextra -Werror -c "$f" -o "$out/h.o" \
                2> "$out/h.err" || { cat "$out/h.err"; fail "<$h> for $p at $O"; }
            n=$((n + 1))
        done
    done
done
"$EMBCC" --target=thumbv7m-none-eabi -c "$out/avr_io.h.c" -o "$out/h.o" 2> "$out/arm.err" &&
    fail "<avr/io.h> was found for ARM"
grep -q 'cannot find include file "avr/io.h"' "$out/arm.err" ||
    { cat "$out/arm.err"; fail "<avr/io.h> off AVR is not 'cannot find'"; }
cat > "$out/asm.S" <<'EOF'
#include <avr/io.h>
	out	_SFR_IO_ADDR(PORTB), r24
	sbi	_SFR_IO_ADDR(DDRB), DDB5
	sts	UDR0, r24
	lds	r24, TCCR1B
	in	r24, _SFR_IO_ADDR(SREG)
EOF
"$EMBCC" --target=avr -c "$out/asm.S" -o "$out/asm.o" || fail "<avr/io.h> in a .S file"
printf 'out\t0x5, r24\nsbi\t0x4, 0x5\nsts\t0xc6, r24\nlds\tr24, 0x81\nin\tr24, 0x3f\n' > "$out/asm.want"
llvm-objdump -d "$out/asm.o" 2>&1 | grep -E '^ +[0-9a-f]+:' | awk -F'\t' '{ print $2 "\t" $3 }' > "$out/asm.dis"
cmp -s "$out/asm.dis" "$out/asm.want" ||
    { diff "$out/asm.want" "$out/asm.dis"; fail "<avr/io.h> in assembly: wrong numbers"; }
echo "  $n compiles of 11 headers (4 parts, -O0/-Os, -Werror); <avr/io.h> in a .S file; not found for ARM"

# 2. the register file against the datasheet's table
regs() { grep -v '^#' $D/m328p-regs.txt | awk 'NF { print $1, $2 }'; }
bits() {   # bits PART: NAME BIT, without the ones the part lacks
    grep -v '^#' $D/m328p-regs.txt | awk -v p=$1 '{ for (i = 3; i <= NF; i++) {
        split($i, b, ":")
        if ((b[1] == "BODS" || b[1] == "BODSE") && p != "atmega328p" && p != "atmega168p") continue
        if (b[1] == "EEAR9" && (p == "atmega168" || p == "atmega168p")) continue
        print b[1], b[2] } }'
}
for p in $PARTS; do
    { echo '#include <avr/io.h>'
      echo 'const uint16_t emb_addr[] = {'
      regs | awk '{ print "    _SFR_MEM_ADDR(" $1 "),   /* " $2 " */" }'
      echo '};'
      echo 'const uint8_t emb_bits[] = {'
      bits $p | awk '{ print "    " $1 "," }'
      echo '};'
    } > "$out/tab-$p.c"
    "$EMBCC" --target=avr -mmcu=$p -O0 -c "$out/tab-$p.c" -o "$out/tab-$p.o" 2> "$out/tab.err" ||
        { cat "$out/tab.err"; fail "$p: a register or bit of the datasheet is missing"; }
    llvm-objcopy -O binary -j .rodata "$out/tab-$p.o" "$out/tab-$p.bin" || fail "no .rodata"
    nr=$(regs | wc -l | tr -d ' ')
    od -An -v -tu2 -N $((nr * 2)) "$out/tab-$p.bin" | tr -s ' ' '\n' | grep . > "$out/got-addr"
    regs | while read -r _ a; do echo $((a)); done > "$out/want-addr"
    paste -d' ' "$out/want-addr" "$out/got-addr" > "$out/pair-addr"
    regs | paste -d' ' - "$out/pair-addr" | awk '$3 != $4 { print; bad = 1 } END { exit bad }' ||
        fail "$p: register addresses differ from the datasheet (name, address, wanted, got)"
    od -An -v -tu1 -j $((nr * 2)) "$out/tab-$p.bin" | tr -s ' ' '\n' | grep . > "$out/got-bits"
    bits $p | paste -d' ' - "$out/got-bits" | awk '$2 != $3 { print; bad = 1 } END { exit bad }' ||
        fail "$p: bit numbers differ from the datasheet (name, wanted, got)"
done
# what the part does not have is not defined
printf '#include <avr/io.h>\n#ifdef BODS\nhas bods\n#endif\n#ifdef EEAR9\nhas eear9\n#endif\n' > "$out/lack.c"
for pw in atmega328p:'has bods has eear9' atmega328:'has eear9' atmega168p:'has bods' atmega168:''; do
    p=${pw%%:*} w=${pw#*:}
    got=$("$EMBCC" --target=avr -mmcu=$p -E "$out/lack.c" | grep '^has' | tr '\n' ' ' | sed 's/ $//')
    [ "$got" = "$w" ] || fail "$p: has '$got', the datasheet says '$w'"
done
# every register the header names is in the table
"$EMBCC" --target=avr -E -dM "$out/avr_io.h.c" |
    awk '$3 ~ /^_SFR_(IO|MEM)(8|16)\(/ { print $2 }' | sort > "$out/hdr-regs"
regs | awk '{ print $1 }' | sort > "$out/tab-regs"
comm -23 "$out/hdr-regs" "$out/tab-regs" > "$out/extra"
[ -s "$out/extra" ] && { cat "$out/extra"; fail "registers in <avr/io.h> that the datasheet table lacks"; }
echo "  $(regs | wc -l | tr -d ' ') registers and $(bits atmega328p | wc -l | tr -d ' ') bits agree with the datasheet on 4 parts; the header names no other register"

# 3. vectors and memories
{ echo '#include <avr/io.h>'
  grep -v '^#' $D/m328p-vectors.txt | awk '{ print "VEC " $1 " " $2 " " $2 "_num" }'
  echo 'SIZE _VECTORS_SIZE'
} > "$out/vec.c"
"$EMBCC" --target=avr -E "$out/vec.c" | grep -E '^(VEC|SIZE) ' > "$out/vec.got"
{ grep -v '^#' $D/m328p-vectors.txt | awk '{ print "VEC " $1 " __vector_" $1 " " $1 }'
  echo 'SIZE (26 * 4)'
} > "$out/vec.want"
cmp -s "$out/vec.got" "$out/vec.want" ||
    { diff "$out/vec.want" "$out/vec.got"; fail "the vector names are not the datasheet's numbers"; }
# part:FLASHEND:RAMEND:E2END:SIG1:SIG2
for m in atmega328p:0x7FFF:0x08FF:0x03FF:0x95:0x0F atmega328:0x7FFF:0x08FF:0x03FF:0x95:0x14 \
         atmega168p:0x3FFF:0x04FF:0x01FF:0x94:0x0B atmega168:0x3FFF:0x04FF:0x01FF:0x94:0x06; do
    IFS=: read -r p fe re ee s1 s2 <<EOF
$m
EOF
    printf '#include <avr/io.h>\n_Static_assert(FLASHEND == %s && RAMEND == %s && E2END == %s && RAMSTART == 0x100 && XRAMEND == RAMEND && SPM_PAGESIZE == 128 && E2PAGESIZE == 4 && SIGNATURE_0 == 0x1E && SIGNATURE_1 == %s && SIGNATURE_2 == %s, "%s");\n' \
        $fe $re $ee $s1 $s2 $p > "$out/mem.c"
    "$EMBCC" --target=avr -mmcu=$p -fsyntax-only "$out/mem.c" 2> "$out/mem.err" ||
        { cat "$out/mem.err"; fail "$p: its memories are not the datasheet's"; }
done
echo "  25 vectors at the datasheet's numbers; the memories and signature of each part"

# 4. the interface in use
cat > "$out/use.c" <<'EOF'
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <avr/sleep.h>
#include <avr/cpufunc.h>
#include <avr/eeprom.h>
#include <util/delay.h>
volatile uint8_t ticks;
uint8_t ee EEMEM;
const uint16_t tab[] PROGMEM = { 1000, 2000 };
ISR(TIMER1_COMPA_vect) { ticks++; }
ISR(TIMER0_OVF_vect, ISR_NOBLOCK) { ticks += 2; }
ISR(INT0_vect, ISR_NAKED) { reti(); }
EMPTY_INTERRUPT(INT1_vect)
ISR_ALIAS(PCINT0_vect, TIMER1_COMPA_vect);
uint16_t rd(uint8_t i) { return pgm_read_word(&tab[i]); }
const char *msg(void) { return PSTR("hello"); }
void wd(void) { wdt_enable(WDTO_2S); wdt_reset(); wdt_disable(); }
void sl(void) { set_sleep_mode(SLEEP_MODE_PWR_DOWN); cli(); sleep_enable(); sleep_bod_disable(); sei(); sleep_cpu(); sleep_disable(); sleep_mode(); }
void dl(void) { _delay_ms(10); _delay_us(5); _NOP(); _MemoryBarrier(); }
uint8_t eep(void) { eeprom_busy_wait(); return eeprom_is_ready() ? 1 : 0; }
void io(void) { DDRB |= _BV(DDB5); PORTB ^= _BV(PORTB5); loop_until_bit_is_set(UCSR0A, UDRE0); UDR0 = 'x';
    TCNT1 = 0x1234; OCR1A = 999; if (bit_is_clear(PINB, PINB0)) GPIOR0 = 1; uint8_t s = SREG; cli(); SREG = s; }
EOF
for O in -O0 -Os; do
    "$EMBCC" --target=avr -mmcu=atmega328p $O -Wall -Wextra -Werror -c "$out/use.c" -o "$out/use$O.o" \
        2> "$out/use.err" || { cat "$out/use.err"; fail "the interface does not compile at $O"; }
    for v in 11 16 1 2 3; do
        llvm-nm "$out/use$O.o" | grep -q " T __vector_$v\$" || fail "$O: no __vector_$v"
    done
    llvm-objdump -d "$out/use$O.o" > "$out/use$O.dis"
    # the watchdog's timed sequence: wdr, then the two stores back to back
    awk '/wdr$/ { w = NR } /sts	0x60,/ { if (NR == w + 1) a = NR; else if (NR == a + 1) pairs++; else bad = 1 }
         END { exit bad || pairs < 2 }' "$out/use$O.dis" ||
        { grep -A2 wdr "$out/use$O.dis"; fail "$O: WDTCSR's two stores are not adjacent after wdr"; }
    # every delay loop: sbiw then a brne back to it (k = -2 words)
    awk '/sbiw/ { s = 1; next } s && /brne/ { if ($0 !~ /f1 f7/) bad = 1; n++ } { s = 0 }
         END { exit bad || !n }' "$out/use$O.dis" ||
        { grep -A1 sbiw "$out/use$O.dis"; fail "$O: the delay loop does not branch back to its sbiw"; }
done
echo "  ISR, ISR_NOBLOCK, ISR_NAKED, EMPTY_INTERRUPT, ISR_ALIAS, PROGMEM, PSTR, wdt, sleep, delay, eeprom compile warning-free; wdt's stores adjacent, delay loops close"
