#!/bin/sh
# Linker scripts: `embld -T SCRIPT`, on the scripts embedded projects
# actually arrive with.
#
# A firmware project's linker script, not its linker, says where flash
# and RAM are, which sections go where, and what the startup code calls
# the bounds of .data and .bss. Two shapes are linked here and RUN:
#
#   - STM32CubeMX's, with a CMSIS-style startup in C, on QEMU's Cortex-M3
#     board: ENTRY, MEMORY, KEEP, SORT, PROVIDE_HIDDEN, AT> FLASH,
#     LOADADDR, COMMON, a heap-and-stack section made only of `.` moves,
#     /DISCARD/, and an orphan section walked by __start_/__stop_.
#   - a SiFive SDK's, with a three-instruction start in assembly, on
#     QEMU's RISC-V virt board at both widths: REGION_ALIAS, ALIGN() on an
#     output section, (NOLOAD), `. += 4K`, ASSERT, and a file pattern
#     that puts the start first.
#
# Then the layout is read back with llvm-readelf, and the mistakes a
# script can contain are made on purpose: each has to be refused with
# ld's meaning, by name.
set -u
echo "TEST-MARKER ldscript"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
NM=${EMBCC_LLVM_NM:-llvm-nm}
command -v "$RE" >/dev/null 2>&1 || { echo "SKIP: $RE not found"; exit 0; }
command -v "$NM" >/dev/null 2>&1 || { echo "SKIP: $NM not found"; exit 0; }
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
d=tests/golden/ldscript
out=tests/golden/out/ldscript
rm -rf "$out"; mkdir -p "$out"

fail() { echo "FAIL: $*"; exit 1; }
sym() { "$NM" "$1" | awk -v s="$2" '$3 == s { print $1 }'; }
hex() { printf '%d' "0x$1"; }

# ---- Cortex-M: STM32CubeMX's script --------------------------------------
T=thumbv7em-none-eabi
for f in startup prog; do
    "$EMBCC" --target=$T -O2 -c "$d/$f.c" -o "$out/$f.o" ||
        fail "$f.c does not compile"
done
"$EMBCC" --target=$T -O2 -c tests/harness/thumb/io.c -o "$out/io.o" ||
    fail "io.c does not compile"
"$EMBLD" -T "$d/stm32.ld" "$out/startup.o" "$out/prog.o" "$out/io.o" \
    -o "$out/fw.elf" 2> "$out/ld.txt" || { cat "$out/ld.txt"; fail "the STM32 script does not link"; }
"$RE" -lSW "$out/fw.elf" > "$out/layout.txt"

# The vector table at 0, where the processor reads it.
grep -qE '\] \.isr_vector +PROGBITS +00000000 ' "$out/layout.txt" ||
    { cat "$out/layout.txt"; fail ".isr_vector is not at address 0"; }
# .data runs in RAM and is STORED in flash: its segment's load address is
# _sidata, and that is right after the last thing in flash before it.
data_seg=$(awk '$1 == "LOAD" && $3 == "0x20000000"' "$out/layout.txt")
[ -n "$data_seg" ] || { cat "$out/layout.txt"; fail "no segment runs at 0x20000000"; }
data_lma=$(echo "$data_seg" | awk '{ print $4 }')
sidata=$(sym "$out/fw.elf" _sidata)
[ "$(printf '%d' "$data_lma")" = "$(hex "$sidata")" ] ||
    fail ".data is stored at $data_lma but _sidata is 0x$sidata"
# (awk without strtonum: the hex is parsed by hand)
prev_end=$(awk -v lma="$data_lma" '
    function h(s,   i, v) { s = tolower(s); sub(/^0x/, "", s); v = 0
        for (i = 1; i <= length(s); i++)
            v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
        return v }
    $1 == "LOAD" { p = h($4); f = h($5)
                   if (p < h(lma) && p + f > m) m = p + f }
    END { print m + 0 }' "$out/layout.txt")
if [ -n "$prev_end" ] && [ "$prev_end" != 0 ]; then
    [ $(( (prev_end + 3) / 4 * 4 )) = "$(printf '%d' "$data_lma")" ] ||
        fail ".data is stored at $data_lma, not right after the flash contents (end $prev_end)"
fi
# The read-only orphan section is in flash; the heap-and-stack section,
# only `.` moves, takes no flash.
grep -qE '\] cmds +PROGBITS +000' "$out/layout.txt" ||
    { cat "$out/layout.txt"; fail "the const table 'cmds' should be an orphan in flash"; }
grep -qE '\] \._user_heap_stack +NOBITS ' "$out/layout.txt" ||
    { cat "$out/layout.txt"; fail "._user_heap_stack should be NOBITS"; }
e=$(hex "$(sym "$out/fw.elf" end)"); eb=$(hex "$(sym "$out/fw.elf" _ebss)")
[ $((e % 8)) = 0 ] && [ "$e" -ge "$eb" ] || fail "end ($e) should be 8-aligned at or after _ebss ($eb)"
echo "STM32 script: vectors at 0, .data stored after the code at _sidata, orphans and the heap section placed as ld places them"

if command -v "$QARM" >/dev/null 2>&1; then
    EMBCC_QEMU_ARM=$QARM sh tests/harness/thumb/run.sh "$out/fw.elf" > "$out/run.txt" 2>&1
    printf 'hello from flash\n42 41 0 1 2 42 1 1 \n43 done\n' > "$out/want.txt"
    cmp -s "$out/run.txt" "$out/want.txt" ||
        { echo "got:"; cat "$out/run.txt"; echo "want:"; cat "$out/want.txt"
          fail "the STM32-script firmware did not run as its script says"; }
    echo "and it runs on the Cortex-M3: .data copied, .bss zeroed, constructors, the cmds table"
else
    echo "SKIP: $QARM not found; the Cortex-M image was not run"
fi

# ---- RISC-V: a SiFive SDK's script ---------------------------------------
for X in 32 64; do
    T=riscv$X-unknown-elf
    "$EMBCC" --target=$T -c "$d/start.S" -o "$out/start$X.o" || fail "start.S rv$X"
    for f in reset rvprog; do
        "$EMBCC" --target=$T -O2 -c "$d/$f.c" -o "$out/$f$X.o" || fail "$f.c rv$X"
    done
    "$EMBCC" --target=$T -O2 -c tests/harness/riscv/io.c -o "$out/io$X.o" || fail "io.c rv$X"
    # reset.o first on the line: the script, not the order, puts _start first
    "$EMBLD" -T "$d/sifive.ld" "$out/reset$X.o" "$out/start$X.o" \
        "$out/rvprog$X.o" "$out/io$X.o" -o "$out/rv$X.elf" 2> "$out/rld$X.txt" ||
        { cat "$out/rld$X.txt"; fail "the SiFive script does not link at rv$X"; }
    st=$(sym "$out/rv$X.elf" _start)
    [ "$(hex "$st")" = "$(printf '%d' 0x80000000)" ] ||
        fail "rv$X: _start is at 0x$st, not first in rom"
    QR=${EMBCC_QEMU_RISCV:-qemu-system-riscv$X}
    if command -v "$QR" >/dev/null 2>&1; then
        sh tests/harness/riscv/run.sh "$out/rv$X.elf" $X > "$out/rrun$X.txt" 2>&1
        printf 'riscv data\n19088743 52719 0 100 \n' > "$out/rwant.txt"
        cmp -s "$out/rrun$X.txt" "$out/rwant.txt" ||
            { echo "got:"; cat "$out/rrun$X.txt"; fail "the rv$X image did not run as its script says"; }
    fi
done
echo "SiFive script: runs at RV32 and RV64, _start first by file pattern"

# Two sections in a row stored in flash and run from RAM: each AT> takes
# the region's next free byte, so the second is stored after the first.
T=thumbv7em-none-eabi
cat > "$out/two.ld" <<'EOF'
MEMORY { FLASH (rx) : ORIGIN = 0, LENGTH = 64K
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 16K }
SECTIONS {
  .text : { *(.text*) *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM AT> FLASH
  .fast : { LONG(0x11223344) LONG(0x55667788) } > RAM AT> FLASH
}
EOF
printf 'int x = 5;\nint y[3] = { 1, 2, 3 };\nint get(int i) { return x + y[i]; }\n' > "$out/two.c"
"$EMBCC" --target=$T -O2 -c "$out/two.c" -o "$out/two.o" || fail "two.c"
"$EMBLD" -T "$out/two.ld" -e get "$out/two.o" \
    -o "$out/two.elf" 2> "$out/two.txt" || { cat "$out/two.txt"; fail "two.ld"; }
"$RE" -lW "$out/two.elf" | awk '$1 == "LOAD" && $3 ~ /^0x2000/ { print $4, $5 }' > "$out/two-lma.txt"
set -- $(cat "$out/two-lma.txt")
[ $# -ge 4 ] && [ $(( $1 + $2 )) -le $(( $3 )) ] ||
    { cat "$out/two-lma.txt"; fail "the second AT> FLASH section is not stored after the first"; }
"$RE" -x .fast "$out/two.elf" | grep -q '44332211 88776655' ||
    { "$RE" -x .fast "$out/two.elf"; fail "LONG() data should be stored little-endian"; }
echo "two sections stored in flash one after the other, LONG() data little-endian in one of them"

# ---- what ld refuses, refused --------------------------------------------
T=thumbv7em-none-eabi
must_refuse() {         # must_refuse TAG PATTERN SCRIPT [OBJECTS...]
    tag=$1; pat=$2; scr=$3; shift 3
    if "$EMBLD" -T "$scr" "$@" -o "$out/$tag.elf" > "$out/$tag.txt" 2>&1; then
        fail "$tag: the link should have been refused"
    fi
    grep -q "$pat" "$out/$tag.txt" ||
        { cat "$out/$tag.txt"; fail "$tag: the refusal should say '$pat'"; }
}
O="$out/startup.o $out/prog.o $out/io.o"
sed 's/LENGTH = 256K/LENGTH = 512/' "$d/stm32.ld" > "$out/small.ld"
must_refuse overflow "region FLASH overflowed by" "$out/small.ld" $O
sed 's/_Min_Stack_Size = 0x400;/_Min_Stack_Size = 0x40000;/' "$d/stm32.ld" > "$out/stack.ld"
must_refuse heapstack "region RAM overflowed by" "$out/stack.ld" $O
printf 'SECTIONS { .text : { *(.text*) } /DISCARD/ : { *(.rodata*) } }\n' > "$out/disc.ld"
must_refuse discard "discards that section" "$out/disc.ld" "$out/prog.o" "$out/io.o"
printf 'PHDRS { text PT_LOAD; }\nSECTIONS { .text : { *(.text) } }\n' > "$out/phdrs.ld"
must_refuse phdrs "PHDRS is not supported" "$out/phdrs.ld" "$out/io.o"
printf 'SECTIONS {\n  .text : { *(.text) }\n  .data : { *(.data) } > NOWHERE\n}\n' > "$out/noreg.ld"
must_refuse noregion "there is no memory region NOWHERE" "$out/noreg.ld" "$out/io.o"
printf 'SECTIONS {\n  .text : { *(.text) \n' > "$out/open.ld"
must_refuse unclosed "open.ld:.*never closed" "$out/open.ld" "$out/io.o"
printf 'SECTIONS { .text : { *(.text*) } ASSERT(SIZEOF(.text) < 4, "text too big") }\n' > "$out/assert.ld"
must_refuse assert "text too big" "$out/assert.ld" "$out/io.o"
printf 'ENTRY(main)\nSECTIONS { .text 0x1000 : { *(.text*) } }\n' > "$out/orph.ld"
if "$EMBLD" -T "$out/orph.ld" --orphan-handling=error "$out/prog.o" "$out/io.o" \
    -o "$out/orph.elf" > "$out/orph.txt" 2>&1; then
    fail "--orphan-handling=error should refuse the unplaced .data"
fi
grep -q "placed by no rule" "$out/orph.txt" || { cat "$out/orph.txt"; fail "orphan refusal"; }
if command -v clang >/dev/null 2>&1; then
    # a tentative definition from a compiler that still emits COMMON
    printf 'int tentative;\nint get(void) { return tentative; }\n' > "$out/common.c"
    clang --target=$T -fcommon -c "$out/common.c" -o "$out/common.o" 2>/dev/null &&
    printf 'SECTIONS { .text : { *(.text*) } .bss : { *(.bss*) } }\n' > "$out/nocommon.ld" &&
    must_refuse common "places no" "$out/nocommon.ld" "$out/common.o"
fi
echo "and refused: region overflow, a discarded section in use, PHDRS, a missing region, an unclosed block, a failed ASSERT, an orphan under error, COMMON with nowhere to go"

# ---- const tables in sections of their own -------------------------------
# A const-only named section is read-only, so a script's orphan rule puts
# it in flash; one with anything writable in it stays writable.
cat > "$out/sec.c" <<'EOF'
__attribute__((section("ro_tab"), used)) static const int a[2] = { 1, 2 };
__attribute__((section("rw_tab"), used)) static const int b = 3;
__attribute__((section("rw_tab"), used)) static int c = 4;
EOF
"$EMBCC" --target=$T -c "$out/sec.c" -o "$out/sec.o" || fail "sec.c"
"$RE" -S "$out/sec.o" | grep -qE '\] ro_tab +PROGBITS .* A +0' ||
    { "$RE" -S "$out/sec.o"; fail "a const-only named section should be read-only"; }
"$RE" -S "$out/sec.o" | grep -qE '\] rw_tab +PROGBITS .* WA +0' ||
    { "$RE" -S "$out/sec.o"; fail "a named section with a writable object should be writable"; }
echo "a const-only named section is read-only, as gcc makes it"

# ---- `end` and `_end`, and -TFILE in one word ----------------------------
# A C library's sbrk starts the heap at the end of the image: newlib's reads
# `end`, EmbCC's own `_end`. A script names the one its libc wanted, often
# only that one -- EmbLinkRTOS's MPS2 script defines `end` -- and EmbCC's
# printf then failed to link with "undefined symbol '_end'". Each now stands
# in for the other; a script naming neither gets the end of its last
# writable section; one naming both keeps both. The script is given as
# `-TFILE`, one word, as gcc takes it and CMake writes it.
printf 'extern char _end[], end[];\nchar buf[100];\nint x = 5;\nlong get(void) { return (long)_end + (long)end + buf[0] + x; }\n' \
    > "$out/end.c"
"$EMBCC" --target=thumbv7em-none-eabi -c "$out/end.c" -o "$out/end.o" || fail "end.c"
for v in "end = .;" "_end = .;" "" "end = .; _end = 0x20001000;"; do
    cat > "$out/end.ld" << EOF2
ENTRY(get)
MEMORY { FLASH (rx) : ORIGIN = 0, LENGTH = 64K
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 16K }
SECTIONS {
  .text : { *(.text*) *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM AT > FLASH
  .bss : { *(.bss*) *(COMMON) } > RAM
  $v
}
EOF2
    "$EMBCC" --target=thumbv7em-none-eabi -nostdlib "-T$out/end.ld" "$out/end.o" \
        -o "$out/end.elf" > "$out/end.txt" 2>&1 || { cat "$out/end.txt"; fail "linking with '$v'"; }
    e1=$(sym "$out/end.elf" end); e2=$(sym "$out/end.elf" _end)
    bss=$("$RE" -S "$out/end.elf" | sed 's/^ *\[ *[0-9]*\]//' | awk '$1 == ".bss" { print $3, $5 }')
    bend=$(printf '%08x' $(( 0x${bss% *} + 0x${bss#* } )))
    case $v in
    *0x20001000*) [ "$e1" = "$bend" ] && [ "$e2" = 20001000 ] ||
                      fail "both named: end=$e1 _end=$e2, want $bend and 20001000" ;;
    *) [ "$e1" = "$bend" ] && [ "$e2" = "$bend" ] ||
           fail "'$v': end=$e1 _end=$e2, want both $bend (the end of .bss)" ;;
    esac
done
echo "end and _end: each stands in for the other, neither means the end of .bss, both are kept; -TFILE is one word"
