#!/bin/sh
# -mmcu= and avr-gcc's other -m options (src/arch/avr/options.c).
#  1. Each part EmbCC takes predefines its own macro in place of
#     __AVR_ATmega328P__ and nothing else changes -- and where clang is
#     here, its -dM for the same -mmcu= names the same part macro.
#     -mmcu=atmega328p is the table EmbCC has without -mmcu=.
#  2. Another part is refused by name: the avr4 siblings (no jmp/call)
#     with their reason, an unknown part with the list, and -mmcu= on a
#     target that is not AVR.
#  3. The size hints -mrelax and -mcall-prologues are accepted and change
#     no byte of the object; -mdouble=64 and -mint8 are refused by name.
set -u
echo "TEST-MARKER avr-mmcu"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/avr-mmcu
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
echo 'int x;' > "$out/e.c"

"$EMBCC" --target=avr -E -dM "$out/e.c" | sort > "$out/none.txt" ||
    fail "no predefines without -mmcu="
"$EMBCC" --target=avr -mmcu=atmega328p -E -dM "$out/e.c" | sort > "$out/m328p.txt"
cmp -s "$out/none.txt" "$out/m328p.txt" ||
    { diff "$out/none.txt" "$out/m328p.txt"; fail "-mmcu=atmega328p changes the predefines"; }
grep -q '^#define __AVR_ATmega328P__ 1$' "$out/m328p.txt" || fail "no __AVR_ATmega328P__"

# part:macro, the macro clang --target=avr -mmcu=PART defines
for pm in atmega328p:__AVR_ATmega328P__ atmega328:__AVR_ATmega328__ \
          atmega168p:__AVR_ATmega168P__ atmega168:__AVR_ATmega168__; do
    p=${pm%%:*} m=${pm#*:}
    "$EMBCC" --target=avr -mmcu=$p -E -dM "$out/e.c" | sort > "$out/$p.txt" ||
        fail "-mmcu=$p is refused"
    grep -q "^#define $m 1\$" "$out/$p.txt" || fail "-mmcu=$p does not define $m"
    [ "$(grep -c '^#define __AVR_ATmega' "$out/$p.txt")" = 1 ] ||
        fail "-mmcu=$p defines more than one part macro"
    # the rest is the ATmega328P's table, line for line
    grep -v '^#define __AVR_ATmega' "$out/$p.txt" > "$out/$p.rest"
    grep -v '^#define __AVR_ATmega' "$out/m328p.txt" > "$out/m328p.rest"
    cmp -s "$out/$p.rest" "$out/m328p.rest" ||
        { diff "$out/m328p.rest" "$out/$p.rest" | head; fail "-mmcu=$p changes more than the part macro"; }
    if command -v clang >/dev/null 2>&1 &&
       clang --target=avr -mmcu=$p -E -dM -x c /dev/null > "$out/$p.clang" 2>/dev/null; then
        c=$(grep '^#define __AVR_ATmega' "$out/$p.clang")
        [ "$c" = "#define $m 1" ] || fail "clang's -mmcu=$p says '$c', the test says $m"
        clang --target=avr -mmcu=$p -E -dM -x c /dev/null 2>/dev/null |
            grep -E '^#define __AVR_(ARCH__|HAVE_JMP_CALL__|2_BYTE_PC__)' | sort > "$out/$p.core"
        grep -E '^#define __AVR_(ARCH__|HAVE_JMP_CALL__|2_BYTE_PC__)' "$out/$p.txt" > "$out/$p.ours"
        cmp -s "$out/$p.core" "$out/$p.ours" || fail "clang's -mmcu=$p is another core"
    fi
done
command -v clang >/dev/null 2>&1 && ref="clang agrees, " || ref=
echo "  -mmcu=atmega328p/328/168p/168: each its own part macro and nothing else; ${ref}none is the ATmega328P"

# 2. refused by name
for p in atmega88 atmega48pa; do
    "$EMBCC" --target=avr -mmcu=$p -c "$out/e.c" -o "$out/x.o" 2> "$out/$p.err" &&
        fail "-mmcu=$p was taken"
    grep -q "the $p is an avr4 part, with no jmp or call" "$out/$p.err" ||
        { cat "$out/$p.err"; fail "-mmcu=$p is not refused for being avr4"; }
done
"$EMBCC" --target=avr -mmcu=atmega2560 -c "$out/e.c" -o "$out/x.o" 2> "$out/2560.err" &&
    fail "-mmcu=atmega2560 was taken"
grep -q 'mmcu=atmega2560 is not supported: .*atmega328p, atmega328, atmega168p and atmega168 alone' \
    "$out/2560.err" || { cat "$out/2560.err"; fail "an unknown part is not refused by name"; }
"$EMBCC" --target=thumbv7m-none-eabi -mmcu=atmega328p -c "$out/e.c" -o "$out/x.o" \
    2> "$out/arm.err" && fail "-mmcu= was taken for ARM"
grep -q 'mmcu=atmega328p is an AVR option, and the target is thumbv7m' "$out/arm.err" ||
    { cat "$out/arm.err"; fail "-mmcu= off AVR is not refused by name"; }
echo "  avr4 parts, an unknown part and -mmcu= off AVR are refused by name"

# 3. the other -m options
cat > "$out/f.c" <<'EOF'
long g(long a) { return a / 3; }
long f(long a, long b) { return a * b + g(a); }
EOF
"$EMBCC" --target=avr -mmcu=atmega328p -Os -c "$out/f.c" -o "$out/plain.o" 2>/dev/null ||
    fail "f.c does not compile"
"$EMBCC" --target=avr -mmcu=atmega328p -Os -mrelax -mcall-prologues -mdouble=32 \
    -mlong-double=32 -c "$out/f.c" -o "$out/hints.o" 2>/dev/null || fail "the hints are refused"
cmp -s "$out/plain.o" "$out/hints.o" || fail "-mrelax/-mcall-prologues changed the object"
for o in -mdouble=64 -mlong-double=64 -mint8; do
    "$EMBCC" --target=avr $o -c "$out/e.c" -o "$out/x.o" 2> "$out/o.err" && fail "$o was taken"
    grep -q -- "$o is not supported: " "$out/o.err" || { cat "$out/o.err"; fail "$o is not refused by name"; }
done
echo "  -mrelax, -mcall-prologues, -mdouble=32 change nothing; -mdouble=64, -mint8 are refused"
