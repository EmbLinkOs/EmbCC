#!/bin/sh
# AVR's __flash (GCC's address space 1): const data kept in program memory
# and read with LPM, instead of copied into SRAM at start-up.
#  1. tests/golden/avr-flash/prog.c, at -O0/-O1/-O2/-Os, prints the known
#     text on QEMU's ATmega328P: a string, int/long/long long/signed-char
#     tables,
#     structs read member by member, a table of __flash pointers in flash,
#     and a function taking a __flash pointer.
#  2. The tables are in flash: in .progmem.data in the object, in .text in
#     the image, and the image's .data is smaller than the same program's
#     without __flash by exactly the tables' bytes -- the SRAM they no
#     longer take.
#  3. What GCC refuses is refused: a __flash object that is not const or
#     not static, a store through a __flash pointer, an implicit
#     conversion between a __flash and a generic pointer (either way),
#     another address space, and an address space on another target.
set -u
echo "TEST-MARKER avr-flash"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
out=tests/golden/out/avr-flash
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-flash
want='flash ok
29071 2147283647 126 232 
16909060 84281096 -5 
one zero
==END=='
link() {   # link OUT OBJ...
    lo=$1; shift
    "$EMBLD" -e __vectors -Ttext 0x0 -Tdata 0x100 --rom-limit 32768 "$@" -o "$lo"
}

for O in -O0 -O1 -O2 -Os; do
    o=$out/O${O#-O}; mkdir -p "$o"
    "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$o/boot.o" &&
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$o/io.o" &&
    "$EMBCC" --target=avr $O -c lib/rt/avr.c -o "$o/rt.o" &&
    "$EMBCC" --target=avr $O -c $D/prog.c -o "$o/prog.o" || fail "$O: does not compile"
    link "$o/prog.elf" "$o/boot.o" "$o/io.o" "$o/rt.o" "$o/prog.o" || fail "$O: does not link"
    if command -v "$QEMU" >/dev/null 2>&1; then
        EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$o/prog.elf" > "$o/run.txt" 2>&1
        got=$(tr -d '\r' < "$o/run.txt")
        [ "$got" = "$want" ] || { printf '%s\n' "$got" | head -5
            fail "$O: the board prints something else"; }
    fi
done
command -v "$QEMU" >/dev/null 2>&1 && echo "  the tables read right on the board at -O0, -O1, -O2 and -Os" ||
    echo "  (no $QEMU: compiled and linked at every level, not run)"

# 2. in flash, and out of SRAM
o=$out/O2
llvm-readelf -S "$o/prog.o" | grep -q '\.progmem\.data' || fail "no .progmem.data in the object"
for s in msg table big small wide pins names; do
    llvm-objdump -t "$o/prog.elf" | grep -qE "\.text	[0-9a-f]+ $s\$" ||
        { llvm-objdump -t "$o/prog.elf" | grep -E " $s\$"; fail "$s is not in flash (.text)"; }
done
"$EMBCC" --target=avr -O2 -D__flash= -c $D/prog.c -o "$out/ram.o" 2>/dev/null &&
    link "$out/ram.elf" "$o/boot.o" "$o/io.o" "$o/rt.o" "$out/ram.o" ||
    fail "the program without __flash does not build"
dsize() { llvm-readelf -S "$1" | awk '$3==".data" { print $7 }'; }
with=$((0x$(dsize "$o/prog.elf"))); without=$((0x$(dsize "$out/ram.elf")))
tables=$((9 + 12 + 12 + 4 + 16 + 12 + 5 + 4 + 4))
[ $((without - with)) = $tables ] ||
    fail ".data is $with bytes with __flash and $without without: $((without - with)) saved, not the tables' $tables"
echo "  the tables are in .progmem.data and in flash; .data is $tables bytes smaller ($with, not $without)"

# 3. refusals
refuse() {   # refuse NAME MESSAGE SOURCE [TRIPLE]
    printf '%s\n' "$3" > "$out/$1.c"
    "$EMBCC" --target=${4:-avr} -c "$out/$1.c" -o "$out/$1.o" 2> "$out/$1.err" &&
        fail "$1: compiled"
    grep -q "$2" "$out/$1.err" || { cat "$out/$1.err"; fail "$1: not refused with \"$2\""; }
}
refuse nonconst "is __flash and must be const" '__flash char x = 1;'
refuse auto "is __flash and must be static" 'int f(void) { const __flash int y = 2; return y; }'
refuse store "of a __flash location" 'void f(__flash char *p) { *p = 1; }'
refuse togeneric "different address spaces" 'const __flash char t[] = "a"; const char *f(void) { return t; }'
refuse toflash "different address spaces" 'const char s[] = "a"; const __flash char *f(void) { return s; }'
refuse space2 "address space 2 is not supported" 'const __attribute__((address_space(2))) char t[] = "a";'
refuse structcopy "copying a whole __flash" 'struct p { int a, b; }; const __flash struct p t[1] = { { 1, 2 } }; int f(void) { struct p x = t[0]; return x.a; }'
refuse other "address space 1 is not supported for thumbv7m" 'const __attribute__((address_space(1))) char t[] = "a";' thumbv7m-none-eabi
echo "  refused: not const, not static, a store, an implicit conversion either way, a whole-struct copy, address space 2, another target"
