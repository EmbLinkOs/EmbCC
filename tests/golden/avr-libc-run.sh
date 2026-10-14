#!/bin/sh
# The AVR library (lib/avr, built into build/libc/avr/libc.a by
# rt-embedded) and <avr/pgmspace.h>, on QEMU's ATmega328P.
#  1. tests/golden/avr-libc-run/prog.c, at -O0 and -Os under -Wall
#     -Wextra -Werror, reads PROGMEM tables back with pgm_read_byte, _word,
#     _dword, _qword, _float, _ptr and _near, runs every _P string function
#     on strings in flash, and prints with snprintf: the formats in fmt.h
#     must print what the host's C library prints for them, and the avr-libc
#     ones (snprintf_P with a PSTR format, %S, a float as "?", sprintf_P,
#     a truncated snprintf) what avr-libc's do.
#  2. The tables and strings are in flash, not copied to SRAM.
#  3. An image carries only the functions it calls: one _P function pulls
#     in no other, and snprintf no 64-bit division.
set -u
echo "TEST-MARKER avr-libc-run"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
out=tests/golden/out/avr-libc-run
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-libc-run
L=build/libc/avr/libc.a
[ -f $L ] || fail "no $L (make rt-embedded)"
# the library is what lib/avr makes now
for f in lib/avr/*.c; do
    [ $L -nt "$f" ] || fail "$L is older than $f (make rt-embedded)"
done

${CC:-cc} -w -I$D $D/host.c -o "$out/host" || fail "the host program"
"$out/host" > "$out/host.txt" || fail "the host program does not run"
{ cat <<'EOF'
byte 22 fe word 1234 beef
dword 89abcdef qword 102030405060708 float 5
ptr world
near 11 33
memcpy_P Hello strlen_P 12 strnlen_P 4
strcmp_P 1 1 1
strncmp_P 1 strcasecmp_P 1 strncasecmp_P 1
memcmp_P 1 1
strcpy_P+strcat_P worldworld
strncpy_P+strncat_P Helwo
strchr_P 2 strrchr_P 8 memchr_P 5
strstr_P world!
EOF
  cat "$out/host.txt"
  cat <<'EOF'
P:12 world ram|    ?|
sprintf_P 99
snprintf 9 trunc
==END==
EOF
} > "$out/want.txt"

for O in -O0 -Os; do
    e=$out/prog$O.elf
    "$EMBCC" --target=avr -mmcu=atmega328p $O -Wall -Wextra -Werror -I$D $D/prog.c -o "$e" \
        2> "$out/build.err" || { cat "$out/build.err"; fail "$O: does not build"; }
    if command -v "$QEMU" >/dev/null 2>&1; then
        EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$e" > "$e.out" 2>&1
        tr -d '\r' < "$e.out" > "$e.txt"
        cmp -s "$e.txt" "$out/want.txt" ||
            { diff "$out/want.txt" "$e.txt" | head -20; fail "$O: the board prints something else"; }
    fi
done
command -v "$QEMU" >/dev/null 2>&1 && ran="on the board" || ran="(no $QEMU: built, not run)"
echo "  pgm_read_*, the 16 _P functions and the printf family at -O0 and -Os: $ran, the formats as the host prints them"

# 2. in flash
e=$out/prog-Os.elf
for v in bytes words dwords qwords floats s1 s2 names; do
    llvm-objdump -t "$e" | grep -qE "\.text	[0-9a-f]+ $v\$" ||
        { llvm-objdump -t "$e" | grep -E " $v\$"; fail "$v is not in flash"; }
done
echo "  the PROGMEM tables and strings are in flash"

# 3. only what is called
printf '#include <avr/pgmspace.h>\nstatic const char s[] PROGMEM = "x";\nint main(void) { return (int)strlen_P(s); }\n' > "$out/one.c"
"$EMBCC" --target=avr -mmcu=atmega328p -Os "$out/one.c" -o "$out/one.elf" 2>/dev/null || fail "strlen_P alone"
n=$(llvm-nm "$out/one.elf" | grep -cE ' T (mem|str)[a-z]*_P$')
[ "$n" = 1 ] || { llvm-nm "$out/one.elf" | grep _P; fail "strlen_P brought $n _P functions"; }
printf '#include <stdio.h>\nchar b[8];\nint main(void) { return snprintf(b, sizeof b, "%%lld", 1LL); }\n' > "$out/pf.c"
"$EMBCC" --target=avr -mmcu=atmega328p -Os "$out/pf.c" -o "$out/pf.elf" 2>/dev/null || fail "snprintf alone"
llvm-nm "$out/pf.elf" | grep -qE ' T __(u?divdi3|udivmoddi4)$' && fail "snprintf of a long long pulled in 64-bit division"
echo "  one _P function brings no other; snprintf needs no 64-bit division"
