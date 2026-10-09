#!/bin/sh
# A struct's size and alignment written into an object as text, the way
# Linux's asm-offsets and EmbLinkRTOS's layout probes do it:
#
#   __asm__ volatile("\n.ascii \"->EMB_PROBE " #name " %c0 %c1\"\n.p2align 2\n"
#                    : : "i"(size), "i"(align))
#
# and a tool reads `->EMB_PROBE name 24 8` out of the ELF, Mach-O or COFF
# object. It works with gcc and clang; with EmbCC it failed on every
# target -- ".ascii is not supported", "modifier '%c' is not supported",
# "does not begin with an instruction".
#
# For each target and at -O0, -O2 and -Os:
#   - the object's code holds both probes with the right numbers (the
#     sizes are C's, the same on every target here but AVR, whose
#     alignment is one -- and the same as clang's);
#   - the second probe of a pair starts on four in the SECTION, wherever
#     the first one landed: the functions shift the first by 0..3 nops,
#     so a `.p2align 2` padded relative to the template, not the
#     section, puts the second off four in at least one of them;
#   - the object is well formed (llvm-objdump reads it whole), and in an
#     ARM or AArch64 ELF object each probe starts on a `$d` mapping
#     symbol and the code after it on `$t`, `$a` or `$x`.
# On Cortex-M and RISC-V a function holding the probes in a branch not
# taken, followed by real code, RUNS on QEMU: the code after the probes is
# reached by the branch around them, so it must sit on an instruction
# boundary -- what the padding is for.
#
# ARMv6-M and ARMv8-M Baseline refuse the probe by name for now: their
# backend (src/arch/thumb/v6m.c) copies a template's bytes as they are,
# and does not yet pad an alignment where the template lands.
set -u
echo "TEST-MARKER asm-layout-marker"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/asm-layout-marker
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
LOBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
LREADELF=${EMBCC_LLVM_READELF:-llvm-readelf}
command -v "$OBJCOPY" >/dev/null 2>&1 && command -v "$LOBJDUMP" >/dev/null 2>&1 || {
    echo "SKIP: llvm-objcopy/llvm-objdump not found"; exit 0; }

# The probe, and a phase shifter: one instruction (or, on RISC-V with
# compressed instructions, a c.nop written as its halfword) that moves
# what follows by the smallest step code can take on that target.
cat > "$out/probe.h" <<'CEOF'
#define EMB_LAYOUT_MARKER(name, size, align) \
    __asm__ __volatile__("\n.ascii \"->EMB_PROBE " #name " %c0 %c1\"\n.p2align 2\n" \
                         : : "i"(size), "i"(align))
struct a { char c; long long x; int y; };
struct b { short s[3]; };
#define PAIR() do { \
    EMB_LAYOUT_MARKER(a, sizeof(struct a), _Alignof(struct a)); \
    EMB_LAYOUT_MARKER(b, sizeof(struct b), _Alignof(struct b)); } while (0)
CEOF
cat > "$out/probe.c" <<'CEOF'
#include "probe.h"
void probe0(void) { PAIR(); }
void probe1(void) { __asm__ volatile(STEP); PAIR(); }
void probe2(void) { __asm__ volatile(STEP "\n" STEP); PAIR(); }
void probe3(void) { __asm__ volatile(STEP "\n" STEP "\n" STEP); PAIR(); }
CEOF

# hex of the section's bytes, and the byte offsets of each probe in it
probes() {                                   # probes OBJ SECTION
    "$OBJCOPY" --dump-section="$2=$out/sec.bin" "$1" "$out/sec.tmp" || return 1
    LC_ALL=C grep -a -b -o -- '->EMB_PROBE [a-z]* [0-9]* [0-9]*' "$out/sec.bin"
}

checked=0
for spec in \
    "x86_64-elf .text .byte_0x90 24_8 6_2" \
    "x86_64-apple-darwin __TEXT,__text .byte_0x90 24_8 6_2" \
    "x86_64-w64-mingw32 .text .byte_0x90 24_8 6_2" \
    "aarch64-elf .text nop 24_8 6_2" \
    "arm64-apple-darwin __TEXT,__text nop 24_8 6_2" \
    "thumbv7em-none-eabi .text nop 24_8 6_2" \
    "armv7a-none-eabi .text nop 24_8 6_2" \
    "riscv32-unknown-elf .text .2byte_1 24_8 6_2" \
    "riscv64-unknown-elf .text .2byte_1 24_8 6_2" \
    "avr .text nop 11_1 6_1"
do
    set -- $spec
    t=$1; sec=$2; step=$(echo "$3" | tr _ ' '); wa=$(echo "$4" | tr _ ' ')
    wb=$(echo "$5" | tr _ ' ')
    firsts=
    for opt in -O0 -O2 -Os; do
        o="$out/$t$opt.o"
        "$EMBCC" --target=$t $opt "-DSTEP=\"$step\"" -I"$out" -c "$out/probe.c" \
            -o "$o" 2> "$out/err" || {
            echo "$t $opt: the probes do not compile:"; grep -v windows-abi "$out/err" | head -3
            exit 1; }
        probes "$o" "$sec" > "$out/found" || { echo "$t $opt: no $sec in the object"; exit 1; }
        n=$(grep -c . "$out/found")
        [ "$n" -eq 8 ] || { echo "$t $opt: $n probes in the code, wanted 8:"; cat "$out/found"; exit 1; }
        [ "$(grep -c -- "->EMB_PROBE a $wa\$" "$out/found")" -eq 4 ] &&
        [ "$(grep -c -- "->EMB_PROBE b $wb\$" "$out/found")" -eq 4 ] || {
            echo "$t $opt: the probes do not say 'a $wa' and 'b $wb':"; cat "$out/found"; exit 1; }
        # the b probe follows an a probe and its .p2align 2: four in the section
        while IFS=: read -r at what; do
            case $what in
            *"PROBE a"*) firsts="$firsts $((at % 4))" ;;
            *) [ $((at % 4)) -eq 0 ] || {
                   echo "$t $opt: a probe after a .p2align 2 is at section offset $at, off four:"
                   cat "$out/found"; exit 1; } ;;
            esac
        done < "$out/found"
        "$LOBJDUMP" -d "$o" > "$out/dis" 2> "$out/dis.err" && [ ! -s "$out/dis.err" ] || {
            echo "$t $opt: llvm-objdump does not read the object:"; head -3 "$out/dis.err"; exit 1; }
        # ARM and AArch64 ELF: $d where each probe starts, and the code's
        # own symbol where the code after the b probe (and its padding,
        # data too) resumes
        case $t in
        thumb*|armv7a*|aarch64-elf)
            "$LOBJDUMP" -t "$o" > "$out/syms"
            while IFS=: read -r at what; do
                grep -Eq "^0*$(printf '%x' "$at") .*[[:space:]][\$]d\$" "$out/syms" || {
                    echo "$t $opt: no \$d mapping symbol where the probe at $at starts:"
                    grep ' [$]' "$out/syms"; exit 1; }
                case $what in *"PROBE b"*)
                    e=$(( (at + ${#what} + 3) / 4 * 4 ))
                    grep -Eq "^0*$(printf '%x' "$e") .*[[:space:]][\$][tax]\$" "$out/syms" || {
                        echo "$t $opt: no code mapping symbol at $e, after the probe at $at:"
                        grep ' [$]' "$out/syms"; exit 1; } ;;
                esac
            done < "$out/found"
            ;;
        esac
        checked=$((checked + 1))
    done
    # across the four functions the first probe of a pair lands at more
    # than one phase -- else the test above could not see a template-
    # relative padding (AArch64 code is all on four: one phase is all)
    case $t in
    aarch64-elf|arm64-apple-darwin|armv7a-none-eabi) ;;
    *) [ "$(echo "$firsts" | tr ' ' '\n' | sort -u | grep -c .)" -ge 2 ] || {
           echo "$t: every first probe landed at phase$firsts; the test sees nothing"; exit 1; } ;;
    esac
done
echo "the probes say 'a 24 8' and 'b 6 2' ('a 11 1', 'b 6 1' on AVR), a .p2align 2 after one lands on four in the section, in $checked objects: x86-64 ELF, Mach-O and COFF, AArch64 ELF and Mach-O, ARMv7E-M, A32, RV32, RV64 and AVR at -O0, -O2 and -Os"

# A function in a section of its own: there the padding is relative to
# THAT section, which starts where the function does -- so the function
# must start on four (x86-64 at -Os starts one anywhere), and the section
# say so.
for spec in "x86_64-elf .byte_0x90" "aarch64-elf nop" "thumbv7em-none-eabi nop" \
            "armv7a-none-eabi nop" "riscv32-unknown-elf .2byte_1" \
            "riscv64-unknown-elf .2byte_1" "avr nop"; do
    set -- $spec
    t=$1; step=$(echo "$2" | tr _ ' ')
    o="$out/$t-fs.o"
    "$EMBCC" --target=$t -Os -ffunction-sections "-DSTEP=\"$step\"" -I"$out" \
        -c "$out/probe.c" -o "$o" || { echo "$t -ffunction-sections: does not compile"; exit 1; }
    for f in probe0 probe1 probe2 probe3; do
        probes "$o" ".text.$f" > "$out/found" || { echo "$t: no .text.$f"; exit 1; }
        [ "$(grep -c . "$out/found")" -eq 2 ] || { echo "$t: .text.$f lacks its probes"; exit 1; }
        at=$(grep 'PROBE b' "$out/found" | cut -d: -f1)
        [ $((at % 4)) -eq 0 ] || {
            echo "$t -ffunction-sections: .text.$f's second probe is at $at, off four"; exit 1; }
        al=$("$LREADELF" -S "$o" | grep " \.text\.$f " | awk '{ print $NF }')
        [ -n "$al" ] && [ "$al" -ge 4 ] || {
            echo "$t: .text.$f is aligned to '$al', not four"; exit 1; }
    done
done
echo "in a section of its own, a function holding the probes starts on four, and so do they"

# ARMv6-M and ARMv8-M Baseline: refused by name, not padded for nowhere
for t in thumbv6m thumbv8m.base; do
    if "$EMBCC" --target=$t-none-eabi "-DSTEP=\"nop\"" -I"$out" -c "$out/probe.c" \
         -o "$out/v6.o" 2> "$out/err"; then
        echo "$t: the probe compiled, which this backend cannot pad yet -- update this test"
        exit 1
    fi
    grep -q "does not do yet" "$out/err" || {
        echo "$t: the probe is refused without saying why:"; head -3 "$out/err"; exit 1; }
done
echo "ARMv6-M and ARMv8-M Baseline refuse the probe's .p2align 2 by name"

# ---- running it ----------------------------------------------------------
cat > "$out/run.c" <<'CEOF'
#include "probe.h"
void writec(int c); void puts_(const char *s); void putn(long v);
volatile int never;
/* The probes are data in the code; they are never run, and the code
 * after them is reached by the branch around them -- which lands on an
 * instruction only if the padding put it on one. */
__attribute__((noinline)) static int after(int x)
{
    if (never)
        PAIR();
    return x * 3 + 1;
}
__attribute__((noinline)) static int after2(int x)
{
    __asm__ volatile(STEP);
    if (never)
        EMB_LAYOUT_MARKER(a, sizeof(struct a), _Alignof(struct a));
    x += 2;
    if (never)
        EMB_LAYOUT_MARKER(b, sizeof(struct b), _Alignof(struct b));
    return x * 5;
}
int main(void)
{
    putn(after(13));           /* 40 */
    putn(after2(6));           /* 40 */
    puts_("\n==END==\n");
    return 0;
}
CEOF
ran=0
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
if command -v "$QEMU" >/dev/null 2>&1; then
    T=thumbv7m-none-eabi
    d="$out/thumb"; mkdir -p "$d"
    export EMBCC_THUMB_HARNESS="$PWD/$d"
    for f in boot io; do
        "$EMBCC" --target=$T -c "tests/harness/thumb/$f.c" -o "$d/$f.o" ||
            { echo "the Cortex-M harness does not compile"; exit 1; }
    done
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt "-DSTEP=\"nop\"" -I"$out" -c "$out/run.c" \
            -o "$d/r$opt.o" || { echo "$T $opt: does not compile"; exit 1; }
        sh tests/harness/thumb/link.sh "$d/r$opt.elf" "$d/r$opt.o" ||
            { echo "$T $opt: does not link"; exit 1; }
        sh tests/harness/thumb/run.sh "$d/r$opt.elf" > "$d/r$opt.txt" 2>&1
        got=$(tr -d '\n' < "$d/r$opt.txt" | sed 's/==END==.*//')
        [ "$got" = "40 40 " ] || {
            echo "$T $opt: the code after the probes computed '$got', wanted '40 40 '"
            exit 1; }
    done
    ran=$((ran + 1))
    echo "Cortex-M: the code after the probes runs at -O0, -O1, -O2 and -Os"
else
    echo "SKIP the Cortex-M run: $QEMU absent"
fi
for w in 32 64; do
    QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv$w}
    command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP rv$w: $QEMU absent"; continue; }
    T=riscv$w-unknown-elf
    d="$out/rv$w"; mkdir -p "$d"
    export EMBCC_RISCV_HARNESS="$PWD/$d"
    for f in boot io; do
        "$EMBCC" --target=$T -c "tests/harness/riscv/$f.c" -o "$d/$f.o" ||
            { echo "rv$w: the harness does not compile"; exit 1; }
    done
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt "-DSTEP=\".2byte 1\"" -I"$out" -c "$out/run.c" \
            -o "$d/r$opt.o" || { echo "$T $opt: does not compile"; exit 1; }
        sh tests/harness/riscv/link.sh "$d/r$opt.elf" "$d/r$opt.o" ||
            { echo "$T $opt: does not link"; exit 1; }
        sh tests/harness/riscv/run.sh "$d/r$opt.elf" "$w" > "$d/r$opt.txt" 2>&1
        got=$(tr -d '\n' < "$d/r$opt.txt" | sed 's/==END==.*//')
        [ "$got" = "40 40 " ] || {
            echo "$T $opt: the code after the probes computed '$got', wanted '40 40 '"
            exit 1; }
    done
    ran=$((ran + 1))
    echo "rv$w: the code after the probes runs at -O0, -O1, -O2 and -Os"
done
[ "$ran" -gt 0 ] || echo "SKIP the run half: no QEMU"
