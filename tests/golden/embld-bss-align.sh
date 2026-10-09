#!/bin/sh
# embld's firmware symbols on word boundaries, for every 32-bit firmware
# target.
#
# A startup brings RAM up with word loops -- `for (p = __bss_start; p <
# __bss_end; ) *p++ = 0;` -- and on a core with no unaligned access a word
# store to an address that is not a multiple of four faults. embld kept
# the end of .data (and so __data_end and __bss_start) on a word boundary
# for MIPS, TriCore, Xtensa, PowerPC, RX, SPARC and ColdFire, but not for
# ARM or RISC-V: a Cortex-M0 image whose .data was 37 bytes started its
# .bss loop at 0x20000025 and took a HardFault before main
# (tests/exec/attr-in-specifiers.c on the micro:bit). This links .data of
# every length 1 to 8 with .bss after it and reads the symbols.
set -u
echo "TEST-MARKER embld-bss-align"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/embld-bss-align
rm -rf "${out:?}"; mkdir -p "$out"
command -v llvm-readelf >/dev/null 2>&1 || { echo "SKIP: needs llvm-readelf"; exit 0; }
EMBCC=${EMBCC:-./embcc}
LD=${EMBLD:-./embld}

cat > "$out/start.c" <<'EOF'
extern unsigned __data_start, __data_end, __data_load, __bss_start, __bss_end;
int main(void);
void _start(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    main();
    for (;;) ;
}
EOF
n=0
for t in thumbv6m-none-eabi thumbv7m-none-eabi riscv32-unknown-elf; do
    "$EMBCC" --target=$t -O1 -c "$out/start.c" -o "$out/start.o" || {
        echo "FAIL: $t: the startup does not compile"; exit 1; }
    for len in 1 2 3 4 5 6 7 8; do
        printf 'char d[%d] = { 1 }; int z;\nint main(void) { return d[0] + z; }\n' \
            $len > "$out/p.c"
        "$EMBCC" --target=$t -O1 -c "$out/p.c" -o "$out/p.o" || {
            echo "FAIL: $t: the program does not compile"; exit 1; }
        "$LD" -e _start -Ttext 0x0 -Tdata 0x20000000 "$out/start.o" "$out/p.o" \
            -o "$out/p.elf" 2> "$out/ld.err" || {
            echo "FAIL: $t: .data of $len does not link:"; head -3 "$out/ld.err"; exit 1; }
        llvm-readelf -s "$out/p.elf" > "$out/p.sym"
        for s in __data_end __bss_start; do
            v=$(awk -v s=$s '$NF == s { print $2 }' "$out/p.sym")
            [ -n "$v" ] || { echo "FAIL: $t: no $s"; exit 1; }
            [ $((0x$v % 4)) = 0 ] || {
                echo "FAIL: $t: with $len bytes of .data, $s is 0x$v -- a startup's"
                echo "      word loop from there faults on a core without unaligned access"
                exit 1; }
            n=$((n + 1))
        done
    done
done
echo "embld-bss-align: __data_end and __bss_start are word-aligned for ARMv6-M, ARMv7-M and RV32 ($n checks)"
