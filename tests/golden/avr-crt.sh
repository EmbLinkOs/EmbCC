#!/bin/sh
# The AVR startup, crt<part>.o (lib/avr/crt.S), and what the driver adds
# under -mmcu=, as avr-gcc's does for avr-libc's.
#  1. tests/golden/avr-crt/prog.c -- written against avr-libc's interface
#     alone: USART0 output, a Timer1 compare interrupt through
#     ISR(TIMER1_COMPA_vect), Timer1's overflow left without a handler and
#     caught by ISR(BADISR_vect) -- prints the same text on QEMU's
#     ATmega328P linked three ways, all with --gc-sections: by EmbCC's
#     own script for the part (no -T), by tests/golden/avr-crt/
#     avr5-style.ld (avr-gcc's default script's shape: its data region
#     starts at 0x800060 unless the driver moves .data, and each startup
#     section is placed and then KEEP()ed), and by that script with
#     -Ttext/-Tdata/--rom-limit as EmbLinkRTOS passes them. The text says
#     .data was copied, .bss zeroed, the constructor run, and early.S's
#     .init3 step run before them.
#  2. The vector table at 0: slot 0 jumps to __init, slot 11 to the
#     program's __vector_11, every other slot to __bad_interrupt, which
#     jumps to the program's __vector_default (BADISR_vect).
#  3. .data runs at 0x800100 (the driver's -Tdata; -Tdata 0x100 means the
#     same), --rom-limit bounds the flash with a script, the ATmega168's
#     script has its 1 KB of SRAM, and -nostartfiles leaves the startup out.
set -u
echo "TEST-MARKER avr-crt"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
out=tests/golden/out/avr-crt
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-crt
R=build/libc/avr

# the runtime the driver links is the one these sources make
for p in atmega328p atmega328 atmega168p atmega168; do
    [ -f $R/crt$p.o ] && [ -f $R/$p.ld ] || fail "no $R/crt$p.o or $p.ld (make rt-embedded)"
    "$EMBCC" --target=avr -mmcu=$p -c lib/avr/crt.S -o "$out/crt$p.o" || fail "crt.S for $p"
    cmp -s "$out/crt$p.o" $R/crt$p.o || fail "$R/crt$p.o is stale (make rt-embedded)"
done

want='data 1234 copied bss 0 ctor 7 init3 90
ticks 3 bad 1
==END=='
L1=
L2="-T $D/avr5-style.ld"
L3="-T $D/avr5-style.ld -Wl,-Ttext=0x0 -Wl,-Tdata=0x800100 -Wl,--rom-limit=32256"
for O in -O0 -Os; do
    for how in 1 2 3; do
        eval "L=\$L$how"
        e=$out/prog$O-$how.elf
        # shellcheck disable=SC2086
        "$EMBCC" --target=avr -mmcu=atmega328p $O -Wall -Wextra -Werror $D/prog.c $D/early.S \
            $L -Wl,--gc-sections -o "$e" 2> "$out/link.err" ||
            { cat "$out/link.err"; fail "$O: link $how"; }
        if command -v "$QEMU" >/dev/null 2>&1; then
            EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$e" > "$e.out" 2>&1
            got=$(tr -d '\r' < "$e.out")
            [ "$got" = "$want" ] || { printf '%s\n' "$got" | head -5
                fail "$O: linked by way $how, the board prints something else"; }
        fi
    done
done
command -v "$QEMU" >/dev/null 2>&1 && ran="the same text on the board" || ran="(no $QEMU: not run)"
echo "  linked by the part's script, an avr5-style one, and that with -Ttext/-Tdata/--rom-limit, at -O0 and -Os: $ran"

# 2. the vector table
e=$out/prog-Os-2.elf
addr() { llvm-nm "$e" | awk -v s="$1" '$3 == s { print $1 }'; }
llvm-objdump -d "$e" > "$out/dis" || fail "no disassembly"
init=$(addr __init) v11=$(addr __vector_11) bad=$(addr __bad_interrupt) dflt=$(addr __vector_default)
[ -n "$init" ] && [ -n "$v11" ] && [ -n "$bad" ] && [ -n "$dflt" ] || fail "a startup symbol is missing"
[ "$v11" != "$bad" ] && [ "$dflt" != "$(addr __vectors)" ] || fail "__vector_11 or __vector_default is the startup's"
slot=0
while [ $slot -lt 26 ]; do
    a=$(printf '%x' $((slot * 4)))
    to=$(awk -v a="$a:" '$1 == a && /jmp/ { print $NF }' "$out/dis")
    case $slot in 0) w=$init ;; 11) w=$v11 ;; *) w=$bad ;; esac
    [ -n "$to" ] && [ $((to)) = $((0x$w)) ] ||
        fail "vector $slot at 0x$a jumps to '$to', not 0x$w"
    slot=$((slot + 1))
done
to=$(awk -v a="$(printf '%x' $((0x$bad))):" '$1 == a && /jmp/ { print $NF }' "$out/dis")
[ $((to)) = $((0x$dflt)) ] || fail "__bad_interrupt jumps to '$to', not __vector_default 0x$dflt"
# without BADISR_vect, an unhandled interrupt restarts the program: the
# default __vector_default is the table itself, at 0
printf 'int main(void) { for (;;) { } }\n' > "$out/none.c"
"$EMBCC" --target=avr -mmcu=atmega328p -Os "$out/none.c" -o "$out/none.elf" || fail "an empty program"
dv=$(llvm-nm "$out/none.elf" | awk '$3 == "__vector_default" { print $1 }')
[ -n "$dv" ] && [ $((0x$dv)) = 0 ] || fail "__vector_default is '$dv' without BADISR_vect, not 0"
echo "  26 slots: reset to __init, 11 to the program's __vector_11, the rest to __bad_interrupt, which goes to BADISR_vect (or to 0)"

# 3. where things are
sect() { llvm-readelf -S "$1" | awk -v s="$2" '$2 == s { print $4 } $3 == s { print $5 }'; }
for how in 1 2 3; do
    [ "$(sect $out/prog-Os-$how.elf .data)" = 00800100 ] ||
        fail "linked by way $how, .data is not at 0x800100"
done
"$EMBCC" --target=avr -mmcu=atmega328p -Os $D/prog.c $D/early.S -T $D/avr5-style.ld \
    -Wl,-Tdata=0x100 -o "$out/td.elf" 2>/dev/null || fail "-Tdata 0x100 with a script"
[ "$(sect $out/td.elf .data)" = 00800100 ] || fail "-Tdata 0x100 is not the data space's 0x800100"
"$EMBCC" --target=avr -mmcu=atmega328p -Os $D/prog.c $D/early.S -T $D/avr5-style.ld \
    -Wl,--rom-limit=512 -o "$out/rl.elf" 2> "$out/rl.err" && fail "--rom-limit=512 took the program"
grep -q 'bytes of flash and the part has 512 (--rom-limit)' "$out/rl.err" ||
    { cat "$out/rl.err"; fail "--rom-limit with a script is not reported"; }
printf 'char big[1500];\nint main(void) { big[3] = 1; return big[0]; }\n' > "$out/big.c"
"$EMBCC" --target=avr -mmcu=atmega328p -Os "$out/big.c" -o "$out/big328.elf" ||
    fail "1500 bytes of .bss do not fit the ATmega328P"
"$EMBCC" --target=avr -mmcu=atmega168 -Os "$out/big.c" -o "$out/big168.elf" 2> "$out/big.err" &&
    fail "1500 bytes of .bss fit the ATmega168's 1 KB"
grep -q 'region data overflowed by 476 bytes' "$out/big.err" || { cat "$out/big.err"; fail "the ATmega168's SRAM overflow is not reported"; }
cat > "$out/own.S" <<'EOF'
	.section .vectors,"ax",@progbits
	.globl	__vectors
__vectors:
	rjmp	main
EOF
printf 'int main(void) { for (;;) { } }\n' > "$out/own.c"
"$EMBCC" --target=avr -mmcu=atmega328p -nostartfiles -Os "$out/own.c" "$out/own.S" -o "$out/own.elf" ||
    fail "-nostartfiles with the program's own vectors"
llvm-nm "$out/own.elf" | grep -q __do_copy_data && fail "-nostartfiles linked the startup"
echo "  .data at 0x800100 (and -Tdata 0x100), --rom-limit with a script, the ATmega168's 1 KB, -nostartfiles"
