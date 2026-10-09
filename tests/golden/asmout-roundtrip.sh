#!/bin/sh
# `-S` must emit the OBJECT, as text -- on every target.
#
# EmbCC's -S is not a codegen stage the way GCC's is. The backend has
# already chosen encodings, so -S prints those BYTES as .byte directives
# with the relocations attached explicitly, and an assembler is given
# nothing to choose. That buys a property GCC and Clang cannot offer:
# assembling the -S output reproduces the -c object exactly. This test
# is that property, and it is the only thing that makes -S trustworthy.
#
# It exists because -S was quietly wrong for three targets. The guard
# refused aarch64 and named the reason -- "emitting text that is not the
# object would be worse than refusing" -- and then thumb, riscv32 and
# riscv64 were added after that line and fell straight through it. They
# got the x86-64 disassembler run over their bytes: wrong instruction
# LENGTHS, so wrong .byte grouping, and x86 mnemonics in the comments.
# `.byte 0x13,0x01  # adc (%rcx),%eax` for a RISC-V `addi`.
#
# Three things had to be true per target, and each is checked here by
# construction rather than by reading:
#   * instruction lengths (src/arch/target.c) -- or the bytes regroup
#   * relocation NAMES (asmout.c) -- or the assembler rejects it
#   * the RISC-V pcrel pair, whose low half names the auipc's ADDRESS
#     and not the symbol, spelled with a local label as an assembler does
set -u
echo "TEST-MARKER asmout-roundtrip"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
NM=${EMBCC_LLVM_NM:-llvm-nm}
READELF=${EMBCC_LLVM_READELF:-llvm-readelf}
out=$EMBCC_ROOT/tests/golden/out/asmout-roundtrip
rm -rf "$out"; mkdir -p "$out"

command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "skipped: llvm-mc / llvm-objcopy not found"; exit 0; }

# Chosen to force every relocation kind these targets emit: a call to an
# undefined symbol, a string literal's address, a global's address, a
# pointer slot in .data, and .bss -- and symbols whose names hold UTF-8
# letters, which an assembler reads only in quotes. And the symbols and
# data an object carries beyond its code: a weak function and variable,
# aliases (CMSIS's weak IRQ handlers), constructors, and tables of
# function and string pointers, const and not, and functions placed in
# sections of their own.
cat > "$out/u.c" <<'CEOF'
extern int helper(int);
static const char msg[] = "hello, world";
int table[4] = { 1, 2, 3, 4 };
int scratch[8];
const char *greet(void) { return msg; }
int *where(void) { return table; }
int compute(int x) { return helper(x) + table[1] + scratch[2]; }
long mix(long a, long b) { return a * b - (a >> 3) + (b & 0xff); }
extern int hölper(int);
int zähler = 3;
int größe(int x) { return hölper(x) + zähler; }
void Default_Handler(void) { }
void USART1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
static int sreal(int x) { return x + 1; }
static int salias(int) __attribute__((alias("sreal")));
__attribute__((weak)) int wfn(void) { return 1; }
__attribute__((weak)) int wvar = 7;
__attribute__((constructor)) static void ctor1(void) { zähler++; }
__attribute__((constructor)) static void ctor2(void) { zähler += 2; }
void (*const vectors[])(void) = { USART1_IRQHandler, Default_Handler };
static const char *names[] = { "a", "b" };
const char *pick(int i) { return names[i] + salias(0) + wfn() - 2; }
__attribute__((section(".text.hot"))) int hot(int x) { return compute(x) + 1; }
__attribute__((section(".ramfunc"))) int ram(int x) { return hot(x) * 2; }
__attribute__((section(".text.hot"))) int hot2(int x) { return ram(x) + hot(x); }
CEOF

fail=0
for spec in "x86_64-elf:x86_64:" \
            "aarch64-elf:aarch64:" \
            "thumbv7m-none-eabi:thumbv7m:" \
            "armv7a-none-eabi:armv7a:" \
            "riscv32-unknown-elf:riscv32:-mattr=+m" \
            "riscv64-unknown-elf:riscv64:-mattr=+m" \
            "avr:avr:-mcpu=atmega328p"; do
    t=${spec%%:*}; rest=${spec#*:}; mc=${rest%%:*}; attr=${rest#*:}
    d="$out/$t"; mkdir -p "$d"

    "$EMBCC" --target="$t" -S "$out/u.c" -o "$d/u.s" 2> "$d/s.err" || {
        echo "FAIL $t: -S refused"; sed 's/^/     | /' "$d/s.err"; fail=1
        continue; }
    "$EMBCC" --target="$t" -c "$out/u.c" -o "$d/direct.o" 2>/dev/null || {
        echo "FAIL $t: -c failed on the same unit"; fail=1; continue; }

    # shellcheck disable=SC2086
    "$MC" -triple="$mc" $attr -filetype=obj "$d/u.s" -o "$d/reasm.o" \
        2> "$d/mc.err" || {
        echo "FAIL $t: llvm-mc rejected the -S output"
        head -4 "$d/mc.err" | sed 's/^/     | /'; fail=1; continue; }

    "$OBJCOPY" -O binary --only-section=.text "$d/direct.o" "$d/direct.bin" \
        2>/dev/null
    "$OBJCOPY" -O binary --only-section=.text "$d/reasm.o"  "$d/reasm.bin" \
        2>/dev/null
    if cmp -s "$d/direct.bin" "$d/reasm.bin"; then
        echo "  $t: .text reassembles byte-identically ($(wc -c < "$d/direct.bin" | tr -d ' ') bytes)"
    else
        echo "FAIL $t: the reassembled .text differs from the object"
        od -An -tx1 "$d/direct.bin" > "$d/a.txt"
        od -An -tx1 "$d/reasm.bin"  > "$d/b.txt"
        diff "$d/a.txt" "$d/b.txt" | head -8 | sed 's/^/     | /'
        fail=1
    fi
    # The data sections byte for byte -- but for ARM's: an ARM assembler
    # keeps a relocation's addend in the bytes (REL), where EmbCC's
    # object keeps it in the relocation (RELA); the relocations below
    # cover both.
    for sec in .data .rodata .init_array .text.hot .ramfunc; do
        [ "$mc" = thumbv7m -o "$mc" = armv7a ] &&
            [ $sec = .data -o $sec = .rodata ] && continue
        # (and in ARM state an assembler rewrites a word naming a static
        # function as its section plus an offset, kept in the bytes; a
        # Thumb function's symbol it keeps, for its bit)
        [ "$mc" = armv7a ] && [ $sec = .init_array ] && continue
        "$OBJCOPY" -O binary --only-section=$sec "$d/direct.o" "$d/a.bin" 2>/dev/null
        "$OBJCOPY" -O binary --only-section=$sec "$d/reasm.o" "$d/b.bin" 2>/dev/null
        cmp -s "$d/a.bin" "$d/b.bin" || {
            echo "FAIL $t: the reassembled $sec differs from the object"; fail=1; }
    done
    # Every defined symbol: name, kind (weak, local, function...) and value.
    for o in direct reasm; do
        "$NM" "$d/$o.o" | grep -v ' \.L' | grep -v ' [tTdDbBrR] $' | sort > "$d/$o.nm"
        # the relocations' places and kinds, per section. Not their
        # symbols: an assembler rewrites one against a local symbol as
        # its section plus an offset, which links the same.
        "$READELF" -r "$d/$o.o" |
            awk '/^Relocation section/ { sec = $3 }
                 /^ *[0-9a-f]+ +[0-9a-f]+ +R_/ { print sec, $1, $3 }' |
            sed 's/\.rela\{0,1\}\./ ./' | sort > "$d/$o.rel"
    done
    diff "$d/direct.nm" "$d/reasm.nm" > /dev/null || {
        echo "FAIL $t: the reassembled symbols differ from the object's"
        diff "$d/direct.nm" "$d/reasm.nm" | head -6 | sed 's/^/     | /'; fail=1; }
    diff "$d/direct.rel" "$d/reasm.rel" > /dev/null || {
        echo "FAIL $t: the reassembled relocations differ from the object's"
        diff "$d/direct.rel" "$d/reasm.rel" | head -6 | sed 's/^/     | /'; fail=1; }
done

# And the grouping itself, which is what silently broke: a fixed-width
# target must not produce a .byte line of the wrong width. RISC-V
# without the C extension is 4 bytes per instruction, always.
# `grep -c` prints 0 AND exits 1 when nothing matches, so `|| echo 0`
# would append a second zero and the comparison below would see "0\n0".
rv=$out/riscv64-unknown-elf/u.s
n=$(grep -c '^	\.byte	0x..,0x..,0x..,0x..$' "$rv" 2>/dev/null) || n=0
bad=$(grep -c '^	\.byte	0x..$' "$rv" 2>/dev/null) || bad=0
if [ "$n" -gt 0 ] && [ "$bad" -eq 0 ]; then
    echo "  riscv64: every instruction line is 4 bytes wide ($n of them)"
else
    echo "FAIL riscv64: instruction grouping is wrong ($n four-byte, $bad single-byte)"
    fail=1
fi

# ARMv7-A is fixed 32-bit too (A32), and grouped by Thumb's halfword rule
# it put each relocation two bytes into the instruction before its own.
a7=$out/armv7a-none-eabi/u.s
n=$(grep -c '^	\.reloc	\.-4, R_ARM_' "$a7" 2>/dev/null) || n=0
bad=$(grep -c '^	\.reloc	\.-[^4],' "$a7" 2>/dev/null) || bad=0
if [ "$n" -gt 0 ] && [ "$bad" -eq 0 ]; then
    echo "  armv7a: every relocation is on its own 4-byte instruction ($n of them)"
else
    echo "FAIL armv7a: instruction grouping is wrong ($n relocations at .-4, $bad elsewhere)"
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
