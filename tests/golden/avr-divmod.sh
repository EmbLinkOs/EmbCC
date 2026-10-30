#!/bin/sh
# avr-gcc's division routines in EmbCC's AVR runtime (lib/rt/avrdivmod.c):
# __udivmodqi4, __divmodqi4, __udivmodhi4, __divmodhi4, __udivmodsi4 and
# __divmodsi4, which an object compiled by avr-gcc or clang calls for a /
# and a % -- each under its own register convention. A program clang
# compiles for the ATmega328P (tests/golden/avr-divmod/dm.c) is linked
# against the runtime and run on EmbSim's uno at -O0 and -O2: 400 pairs at
# each width, signed and unsigned, against a long division in shifts. It
# did not link before ("undefined symbol '__udivmodsi4'").
set -u
echo "TEST-MARKER avr-divmod"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBSIM=${EMBSIM:-./embsim}
CLANG=${EMBCC_CLANG:-clang}
out=tests/golden/out/avr-divmod
rm -rf "${out:?}"; mkdir -p "$out/h"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
printf 'int f(int a, int b) { return a / b; }\n' |
    "$CLANG" --target=avr -mmcu=atmega328p -x c -c - -o "$out/probe.o" 2>/dev/null ||
    { echo "SKIP: $CLANG has no AVR backend"; exit 0; }

"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/h/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/h/io.o" &&
sh tools/build-rt.sh avr "$out/h" > "$out/h/rt.log" 2>&1 ||
    fail "the harness does not build"
for f in __udivmodqi4 __divmodqi4 __udivmodhi4 __divmodhi4 __udivmodsi4 __divmodsi4; do
    llvm-nm "$out/h/librt.a" 2>/dev/null | grep -q " W $f\$" || fail "librt.a has no $f"
done

for O in -O0 -O2; do
    "$CLANG" --target=avr -mmcu=atmega328p $O -w -c tests/golden/avr-divmod/dm.c \
        -o "$out/dm$O.o" || fail "$O: clang does not compile dm.c"
    n=$(llvm-nm "$out/dm$O.o" | grep -cE ' U __(u)?divmod[qhs]i4$')
    [ "$n" -ge 4 ] || fail "$O: clang's object calls $n of the routines, want 4 or more"
    EMBCC_AVR_HARNESS="$out/h" sh tests/harness/avr/link.sh "$out/dm$O.elf" "$out/dm$O.o" "$out/h/librt.a" \
        > "$out/dm$O.lerr" 2>&1 || { cat "$out/dm$O.lerr"; fail "$O: does not link"; }
    "$EMBSIM" "$out/dm$O.elf" --board uno --until '==END==' --max-insns 50000000 \
        > "$out/dm$O.out" 2>&1
    [ "$(head -1 "$out/dm$O.out" | tr -d ' ')" = 42 ] ||
        { head -3 "$out/dm$O.out"; fail "$O: a quotient or a remainder is wrong (bits: qi hi si, then signed)"; }
done
echo "avr-divmod: clang's AVR objects link against librt's __udivmodqi4 ... __divmodsi4 and divide right at every width, at -O0 and -O2"
