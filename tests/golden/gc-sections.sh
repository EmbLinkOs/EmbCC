#!/bin/sh
# -ffunction-sections, -fdata-sections and --gc-sections: each function
# and object in a section of its own, and the linker dropping every
# section nothing kept refers to -- the way firmware is built to fit.
#
# Every image here RUNS, because what a collector gets wrong is what it
# keeps too little of: a handler reached only from the vector table, a
# function reached only through a table in .rodata, a constructor, a
# section walked by __start_/__stop_. Each would link and then do
# nothing. So:
#
#   - the STM32-shaped script (KEEP, .init_array, an orphan table walked
#     by its bounds) on Cortex-M, with -Map and --print-memory-usage
#     checked against what llvm-readelf reads from the same image;
#   - each board's harness link with no script, at -O0 and -O2: Cortex-M,
#     RV32, RV64 and the ATmega328P, which keeps .rodata in RAM;
#   - x86-64 with unwind tables, where .eh_frame describes every
#     function and must neither keep them all nor describe one that went;
#   - -g: DW_AT_ranges for a unit split over sections, read back by
#     llvm-dwarfdump --verify, in the object and in the image.
set -u
echo "TEST-MARKER gc-sections"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
EMBAR=${EMBAR:-./embar}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
NM=${EMBCC_LLVM_NM:-llvm-nm}
DD=${EMBCC_LLVM_DWARFDUMP:-llvm-dwarfdump}
command -v "$RE" >/dev/null 2>&1 || { echo "SKIP: $RE not found"; exit 0; }
command -v "$NM" >/dev/null 2>&1 || { echo "SKIP: $NM not found"; exit 0; }
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
QAVR=${EMBCC_QEMU_AVR:-qemu-system-avr}
d=tests/golden/gc-sections
L=tests/golden/ldscript
out=tests/golden/out/gc-sections
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
SEC="-ffunction-sections -fdata-sections"
# has IMAGE SYM..., lacks IMAGE SYM...: by llvm-nm, defined symbols only
has() {
    img=$1; shift
    for s in "$@"; do
        "$NM" --defined-only "$img" | awk '{ print $3 }' | grep -qx "$s" ||
            fail "$img: '$s' is not in the image"
    done
}
lacks() {
    img=$1; shift
    for s in "$@"; do
        if "$NM" --defined-only "$img" | awk '{ print $3 }' | grep -qx "$s"; then
            fail "$img: '$s' is in the image, and nothing reaches it"
        fi
    done
}
# A wrapper that adds --gc-sections to a harness's own link line.
printf '#!/bin/sh\nexec "%s" --gc-sections "$@"\n' "$PWD/$EMBLD" > "$out/embld-gc"
chmod +x "$out/embld-gc"
GCLD=$PWD/$out/embld-gc

# ---- Cortex-M, the STM32CubeMX-shaped script ------------------------------
T=--target=thumbv7em-none-eabi
for f in startup prog; do
    "$EMBCC" $T -O2 $SEC -c "$L/$f.c" -o "$out/$f.o" || fail "$f.c"
done
"$EMBCC" $T -O2 $SEC -c tests/harness/thumb/io.c -o "$out/io.o" || fail io.c
"$EMBCC" $T -O2 $SEC -c "$d/extra.c" -o "$out/extra.o" || fail extra.c
"$EMBAR" rcs "$out/libio.a" "$out/io.o" || fail "embar"
"$RE" -S "$out/prog.o" | grep -q ' \.text\.main ' ||
    fail "-ffunction-sections did not put main in .text.main"
"$RE" -S "$out/prog.o" | grep -q ' \.bss\.zeros ' ||
    fail "-fdata-sections did not put zeros in .bss.zeros"
"$RE" -S "$out/extra.o" | grep -q ' \.rodata\.unused_table ' ||
    fail "-fdata-sections did not put a const object in .rodata.NAME"
# A function section claims the alignment the function has -- two on
# Thumb -- or the link pads every function out to a fixed sixteen.
al=$("$RE" -S "$out/extra.o" | awk '/ \.text\.unused_fn / { print $NF }')
[ "$al" = 2 ] || fail ".text.unused_fn claims alignment $al; a Thumb function needs 2"

"$EMBCC" $T -T "$L/stm32.ld" "$out/startup.o" "$out/prog.o" "$out/io.o" \
    "$out/extra.o" -o "$out/full.elf" || fail "the link without --gc-sections"
"$EMBCC" $T -T "$L/stm32.ld" \
    -Wl,--gc-sections,--print-gc-sections,-u,kept_by_u \
    -Wl,-Map="$out/fw.map",--print-memory-usage \
    "$out/startup.o" "$out/prog.o" "$out/extra.o" -L"$out" -lio \
    -o "$out/fw.elf" > "$out/usage.txt" 2> "$out/gc.txt" ||
    { cat "$out/gc.txt"; fail "the link with --gc-sections"; }
has "$out/fw.elf" g_pfnVectors Reset_Handler Default_Handler main puts_ \
    putn counter greeting kept_by_u kept_data
lacks "$out/fw.elf" unused_fn unused_caller unused_counter unused_table \
    unused_zero
has "$out/full.elf" unused_fn unused_table
# --print-gc-sections, in ld's words, names what went and nothing else
sed "s|$out/||" "$out/gc.txt" > "$out/gc.n"
for s in .text.unused_fn .text.unused_caller .data.unused_counter \
         .rodata.unused_table .bss.unused_zero; do
    grep -qx "embld: removing unused section '$s' in file 'extra.o'" \
        "$out/gc.n" || fail "--print-gc-sections did not name $s"
done
if grep -E "'(\.isr_vector|\.text\.main|\.text\.init_first|\.init_array|cmds|\.text\.kept_by_u|\.data\.kept_data|\.text\.Reset_Handler)'" \
        "$out/gc.n"; then
    fail "a section something keeps was collected"
fi
fsz() { "$RE" -l "$1" | awk '$1 == "LOAD" && $4 ~ /^0x000/ { n = strtonum($4) + strtonum($5); if (n > m) m = n } END { print m }'; }
run_m3() {  # run_m3 IMAGE WANT
    command -v "$QARM" >/dev/null 2>&1 || return 0
    sh tests/harness/qrun.sh 10 --until done "$QARM" -M lm3s6965evb \
        -cpu cortex-m3 -nographic -kernel "$1" > "$1.txt" 2>/dev/null
    # (stderr: the harness image stops by locking the core up on purpose)
    tr -d '\r' < "$1.txt" | head -n "$(printf "$2" | wc -l)" > "$1.got"
    printf "$2" | cmp -s - "$1.got" || { cat "$1.txt"; fail "$1 did not run as built"; }
}
run_m3 "$out/fw.elf" 'hello from flash\n42 41 0 1 2 42 1 1 \n43 done\n'
echo "Cortex-M, STM32 script: unused code and data of four kinds gone; the"
echo "vector table, a constructor, a __start_/__stop_ table and -u kept; runs"

# --print-memory-usage: ld's table, and the FLASH figure is the end of what
# the image stores there, as llvm-readelf reads the program headers
head -1 "$out/usage.txt" | grep -qx 'Memory region         Used Size  Region Size  %age Used' ||
    { cat "$out/usage.txt"; fail "--print-memory-usage: not ld's header"; }
grep -Eq '^ +RAM: +[0-9]+ B +64 KB +[0-9]+\.[0-9]{2}%$' "$out/usage.txt" &&
grep -Eq '^ +FLASH: +[0-9]+ B +256 KB +[0-9]+\.[0-9]{2}%$' "$out/usage.txt" ||
    { cat "$out/usage.txt"; fail "--print-memory-usage: not ld's rows"; }
used=$(awk '$1 == "FLASH:" { print $2 }' "$out/usage.txt")
stored=$("$RE" -l "$out/fw.elf" | awk '$1 == "LOAD" { a = $4; s = $5; sub(/^0x/, "", a); sub(/^0x/, "", s); e = 0; n = length(a); for (i = 1; i <= n; i++) e = e * 16 + index("0123456789abcdef", substr(a, i, 1)) - 1; z = 0; n = length(s); for (i = 1; i <= n; i++) z = z * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; if (e < 262144 && e + z > m) m = e + z } END { print m }')
[ "$used" = "$stored" ] ||
    fail "--print-memory-usage says FLASH holds $used bytes; the program headers end at $stored"
full=$("$EMBCC" $T -T "$L/stm32.ld" -Wl,--print-memory-usage "$out/startup.o" \
    "$out/prog.o" "$out/io.o" "$out/extra.o" -o "$out/full2.elf" |
    awk '$1 == "FLASH:" { print $2 }')
[ "$full" -gt "$used" ] || fail "--gc-sections saved nothing ($full and $used bytes)"
echo "--print-memory-usage: ld's table; FLASH $used bytes with --gc-sections, $full without"

# -Map: ld's sections, and the addresses the image has
for l in 'Archive member included to satisfy reference by file (symbol)' \
         'Discarded input sections' 'Memory Configuration' \
         'Linker script and memory map'; do
    grep -qx "$l" "$out/fw.map" || fail "-Map: no '$l'"
done
grep -Eq '^FLASH +0x00000000 +0x00040000 +rx$' "$out/fw.map" ||
    fail "-Map: the FLASH region is not listed as the script has it"
# (a name longer than ld's column ends its line, as in ld's maps)
grep -A1 -x ' \.text\.unused_fn' "$out/fw.map" |
    grep -Eq '^ {16}0x00000000 +0x[0-9a-f]+ .*extra\.o$' ||
    fail "-Map: .text.unused_fn is not among the discarded"
taddr=$("$NM" "$out/fw.elf" | awk '$3 == "main" { print $1 }')
grep -A1 -E '^ \.text\.main' "$out/fw.map" | grep -Eq "0x$taddr" ||
    fail "-Map: .text.main is not where the image has main (0x$taddr)"
grep -Eq "^ +0x$(printf '%08x' $((0x$taddr | 1))) +main$" "$out/fw.map" ||
    fail "-Map: main's symbol line"
grep -Eq '^\.data +0x20000000 +0x[0-9a-f]+ load address 0x[0-9a-f]+$' "$out/fw.map" ||
    fail "-Map: .data's load address"
# the archive member, and the object and symbol it was pulled in for
grep -A1 -F "$out/libio.a(io.o)" "$out/fw.map" |
    grep -Eq "^ {30}$out/prog\.o \(put(s_|n)\)$" ||
    { head -8 "$out/fw.map"; fail "-Map: why libio.a(io.o) was pulled"; }
echo "-Map: discarded, memory configuration, sections with their inputs and symbols"

# -g: the unit is in several sections, so its extent is DW_AT_ranges
if command -v "$DD" >/dev/null 2>&1; then
    for f in startup prog; do
        "$EMBCC" $T -O2 -g $SEC -c "$L/$f.c" -o "$out/g$f.o" || fail "-g $f.c"
    done
    "$DD" --debug-info "$out/gprog.o" | grep -q DW_AT_ranges ||
        fail "-g: a split unit has no DW_AT_ranges"
    "$DD" --verify "$out/gprog.o" > "$out/v1.txt" 2>&1 ||
        { cat "$out/v1.txt"; fail "-g: llvm-dwarfdump rejects the object"; }
    "$EMBCC" $T -T "$L/stm32.ld" -Wl,--gc-sections "$out/gstartup.o" \
        "$out/gprog.o" "$out/io.o" -o "$out/g.elf" 2> /dev/null ||
        fail "-g: the link"
    "$DD" --verify "$out/g.elf" > "$out/v2.txt" 2>&1 ||
        { cat "$out/v2.txt"; fail "-g: llvm-dwarfdump rejects the image"; }
    m=$("$NM" "$out/g.elf" | awk '$3 == "main" { print $1 }')
    "$DD" --lookup=0x$m "$out/g.elf" | grep -q 'DW_AT_name.*"main"' ||
        fail "-g: main's address does not look up to main"
    run_m3 "$out/g.elf" 'hello from flash\n42 41 0 1 2 42 1 1 \n43 done\n'
    echo "-g: DW_AT_ranges and per-section addresses, verified in object and image"
fi

# ---- each board's harness, no script --------------------------------------
want='gc 42 15 10 42 \ndone\n'
for O in -O0 -O2; do
    # Cortex-M: .vectors and the entry are the roots
    H=$out/m3$O; mkdir -p "$H"
    for f in boot io; do
        "$EMBCC" $T $O $SEC -c tests/harness/thumb/$f.c -o "$H/$f.o" || fail "m3 $f"
    done
    "$EMBCC" $T $O $SEC -c "$d/gcprog.c" -o "$H/p.o" || fail "m3 gcprog $O"
    EMBLD=$GCLD EMBCC_THUMB_HARNESS=$H sh tests/harness/thumb/link.sh \
        "$H/p.elf" "$H/p.o" || fail "m3 $O: link"
    has "$H/p.elf" main helper table used_data used_zero gc_ctor vectors
    lacks "$H/p.elf" unused_fn unused_caller unused_data unused_ro unused_zero
    run_m3 "$H/p.elf" "$want"

    for X in 32 64; do
        R=--target=riscv$X-unknown-elf
        H=$out/rv$X$O; mkdir -p "$H"
        for f in boot io; do
            "$EMBCC" $R $O $SEC -c tests/harness/riscv/$f.c -o "$H/$f.o" ||
                fail "rv$X $f"
        done
        "$EMBCC" $R $O $SEC -c "$d/gcprog.c" -o "$H/p.o" || fail "rv$X gcprog $O"
        EMBLD=$GCLD EMBCC_RISCV_HARNESS=$H sh tests/harness/riscv/link.sh \
            "$H/p.elf" "$H/p.o" || fail "rv$X $O: link"
        has "$H/p.elf" main helper table
        lacks "$H/p.elf" unused_fn unused_caller unused_data unused_ro
        QR=${EMBCC_QEMU_RISCV:-qemu-system-riscv$X}
        if command -v "$QR" >/dev/null 2>&1; then
            EMBCC_QEMU_RISCV=$QR sh tests/harness/riscv/run.sh "$H/p.elf" $X \
                > "$H/run.txt" 2>&1
            tr -d '\r' < "$H/run.txt" | head -2 > "$H/got.txt"
            printf "$want" | cmp -s - "$H/got.txt" ||
                { cat "$H/run.txt"; fail "rv$X $O did not run as built"; }
        fi
    done

    # ATmega328P: calls always relocated, .rodata in the RAM image
    H=$out/avr$O; mkdir -p "$H"
    "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" || fail "avr boot"
    "$EMBCC" --target=avr $O $SEC -c tests/harness/avr/io.c -o "$H/io.o" || fail "avr io"
    "$EMBCC" --target=avr $O $SEC -c lib/rt/avr.c -o "$H/rt.o" || fail "avr rt"
    "$EMBCC" --target=avr $O $SEC -c "$d/gcprog.c" -o "$H/p.o" || fail "avr gcprog $O"
    EMBLD=$GCLD EMBCC_AVR_HARNESS=$H sh tests/harness/avr/link.sh \
        "$H/p.elf" "$H/p.o" || fail "avr $O: link"
    has "$H/p.elf" main helper table
    lacks "$H/p.elf" unused_fn unused_caller unused_data unused_ro
    if command -v "$QAVR" >/dev/null 2>&1; then
        EMBCC_QEMU_UNTIL=done EMBCC_QEMU_AVR=$QAVR sh tests/harness/avr/run.sh \
            "$H/p.elf" > "$H/run.txt" 2>&1
        tr -d '\r' < "$H/run.txt" | head -2 > "$H/got.txt"
        printf "$want" | cmp -s - "$H/got.txt" ||
            { cat "$H/run.txt"; fail "avr $O did not run as built"; }
    fi
done
echo "harness links with --gc-sections, -O0 and -O2: Cortex-M, RV32, RV64 and"
echo "AVR run, with a function reached only through a table kept"

# ---- x86-64: .eh_frame -----------------------------------------------------
"$EMBCC" -O1 -funwind-tables $SEC -c "$d/eh.c" -o "$out/eh.o" || fail "eh.c"
"$EMBLD" -e _start --gc-sections "$out/eh.o" -o "$out/eh.elf" ||
    fail "x86-64: the link"
lacks "$out/eh.elf" unused_leaf
python3 "$d/fdes.py" "$out/eh.elf" > "$out/fdes.txt" || fail "x86-64: fdes.py"
"$NM" -S "$out/eh.elf" | awk '$4 == "used_leaf" || $4 == "_start" {
    a = $1; s = $2; sub(/^0+/, "", a); sub(/^0+/, "", s); print a, s }' |
    sort > "$out/fns.txt"
grep -v ' 0$' "$out/fdes.txt" | sort > "$out/live.txt"
cmp -s "$out/fns.txt" "$out/live.txt" ||
    { cat "$out/fdes.txt"; fail "x86-64: the FDEs left are not the functions kept"; }
[ "$(grep -c ' 0$' "$out/fdes.txt")" = 1 ] ||
    { cat "$out/fdes.txt"; fail "x86-64: the collected function's FDE should cover nothing"; }
echo "x86-64: .eh_frame kept, its FDEs for kept functions exact, the other covering nothing"

# ---- ARM EHABI: .ARM.exidx lives and dies with its function ---------------
# clang's object (EmbCC writes no exidx): each .ARM.exidx.text.F has
# SHF_LINK_ORDER to .text.F, and R_ARM_NONE on the personality routine.
CLANG=${EMBCC_REF_CLANG:-clang}
if command -v "$CLANG" >/dev/null 2>&1; then
    "$CLANG" --target=thumbv7em-none-eabi -O0 -ffunction-sections \
        -funwind-tables -c "$d/eh.c" -o "$out/exidx.o" || fail "clang: eh.c"
    echo 'void __aeabi_unwind_cpp_pr0(void) {}' > "$out/pr0.c"
    "$EMBCC" $T -c "$out/pr0.c" -o "$out/pr0.o" || fail "pr0.c"
    "$EMBLD" -T "$d/exidx.ld" --gc-sections "$out/exidx.o" "$out/pr0.o" \
        -o "$out/exidx.elf" || fail "exidx: the link"
    lacks "$out/exidx.elf" unused_leaf
    "$RE" -S "$out/exidx.elf" | grep -Eq ' \.ARM\.exidx +ARM_EXIDX ' ||
        fail "exidx: the output section is not ARM_EXIDX"
    "$RE" -u "$out/exidx.elf" | awk '/FunctionAddress/ { print $2 }' |
        sort > "$out/exidx.got"
    for f in used_leaf _start; do
        a=$("$NM" "$out/exidx.elf" | awk -v s=$f '$3 == s { print $1 }')
        printf '0x%x\n' $((0x$a & ~1))
    done | sort > "$out/exidx.want"
    cmp -s "$out/exidx.got" "$out/exidx.want" ||
        { cat "$out/exidx.got"; fail "exidx: the index is not the kept functions'"; }
    echo "ARM EHABI: the index holds the kept functions' entries and no other"
fi

# ---- many sections, long names --------------------------------------------
awk 'BEGIN { for (i = 0; i < 300; i++)
    printf "int function_with_a_name_long_enough_to_be_a_mangled_cxx_name_%03d(int x) { return x + %d; }\n", i, i
    print "int main(void) { return function_with_a_name_long_enough_to_be_a_mangled_cxx_name_299(1); }" }' \
    > "$out/many.c"
"$EMBCC" $T -O1 -g $SEC -c "$out/many.c" -o "$out/many.o" || fail "300 functions"
n=$("$RE" -S "$out/many.o" | grep -c ' \.text\.function_with')
[ "$n" = 300 ] || fail "300 functions made $n sections"
"$RE" -S "$out/many.o" | grep -q ' \.rela\.text\.main ' ||
    "$RE" -S "$out/many.o" | grep -q ' \.rel\.text\.main ' ||
    fail "no relocation section for .text.main"
[ -z "$(command -v "$DD")" ] || "$DD" --verify "$out/many.o" > /dev/null 2>&1 ||
    fail "300 functions: llvm-dwarfdump rejects the object"
echo "300 functions: 300 sections, relocations for each, DWARF that verifies"

# ---- refused ----------------------------------------------------------------
if "$EMBLD" --print-gc-sections -e _start "$out/eh.o" -o "$out/x.elf" \
        2> "$out/r.txt"; then
    fail "--print-gc-sections without --gc-sections should be refused"
fi
grep -q 'without --gc-sections' "$out/r.txt" || { cat "$out/r.txt"; fail "its message"; }
echo "refused: --print-gc-sections without --gc-sections"
