#!/bin/sh
# ARM long-branch veneers: a branch embld's layout puts out of reach goes
# through a stub at the end of the CALLER's output section, as GNU ld
# makes one. The case every Cortex-M firmware has: a function run from
# RAM (0x20000000) calling flash (0x00000000), 512 MB away, and flash
# calling it back -- a Thumb `bl` reaches 16 MB.
#
#   1. Cortex-M3, the default layout (-Tdata 0x20000000): a function in a
#      .data section, copied to RAM by the startup, calls a flash function
#      twice and a static one; main calls it. One veneer per output section
#      and target (two in .data, one in .text), movw/movt/bx ip; the image
#      runs and prints the sums. The veneers move .bss, which is laid out
#      again -- a COMMON (-fcommon) included, which once kept its first
#      address and overlapped the .bss array before it.
#   2. The same through a linker script (*(.ramfunc) in .data > RAM AT >
#      FLASH), which is how a vendor's startup places it.
#   3. Cortex-M0 (ARMv6-M, no movw): push/ldr/str/pop {r0, pc} veneers.
#   4. ARMv7-A, ARM code, -Tdata 112 MB above the code (an ARM `bl` reaches
#      32 MB): `ldr ip, [pc]; bx ip` veneers, both ways.
#   5. Interworking: clang's Thumb `b.w` (a tail call) to an ARM-state
#      function, which no branch can encode, goes through a veneer whose
#      `bx ip` switches state, whatever the distance. This was refused.
set -u
echo "TEST-MARKER embld-veneers"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
out=tests/golden/out/embld-veneers
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU not found"; exit 0; }

cat > "$out/prog.c" <<'EOF'
void putn(long v);
void puts_(const char *s);
__attribute__((noinline)) int flashfn(int x);
__attribute__((noinline)) static int sflash(int x) { return x * 3; }
#ifndef RAMSEC
#define RAMSEC ".data.ramfn"
#endif
__attribute__((section(RAMSEC), noinline)) int ramfn(int x)
{
    return flashfn(x) + flashfn(x + 1) + sflash(x);
}
int flashfn(int x) { return x * 2; }
/* .bss, then a COMMON (-fcommon) after it: the veneers in .data move
 * both, and a COMMON left where the first layout put it overlaps zb */
static int zb[8];
int cm[8];
__attribute__((noinline)) int bss_sum(void)
{
    int s = 0;
    for (int i = 0; i < 8; i++) { zb[i] = 1; cm[i] = 2; }
    for (int i = 0; i < 8; i++) s += zb[i];
    return s;
}
int main(void)
{
    puts_("ram "); putn(ramfn(20)); puts_("\n");
    puts_("again "); putn(ramfn(1)); puts_("\n");
    puts_("bss "); putn(bss_sum()); puts_("\n");
    return 42;
}
EOF
check_out() {   # check_out LOG WHAT
    tr -d '\r' < "$1" > "$1.txt"
    grep -q "^ram 142" "$1.txt" && grep -q "^again 9" "$1.txt" &&
        grep -q "^bss 8" "$1.txt" ||
        fail "$2 did not print ram 142 / again 9 / bss 8: $(head -4 "$1.txt")"
}
# count_veneers ELF SECTION TRIPLE PATTERN: how many veneers the section has
count_veneers() {
    "$OBJDUMP" -d -j "$2" --triple="$3" "$1" | grep -c "$4"
}

# ---- 1. Cortex-M3, the default layout -------------------------------------
T=thumbv7m-none-eabi
d=$out/m3
mkdir -p "$d"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c tests/harness/thumb/$f.c -o "$d/$f.o" || fail "$f.c"
done
"$EMBCC" --target=$T -O2 -fcommon -c "$out/prog.c" -o "$d/prog.o" || fail "prog.c for $T"
EMBCC_THUMB_HARNESS=$d sh tests/harness/thumb/link.sh "$d/prog.elf" "$d/prog.o" \
    > "$d/link.log" 2>&1 || fail "the M3 image does not link: $(head -3 "$d/link.log")"
sh tests/harness/qrun.sh 10 "$QEMU" -M lm3s6965evb -cpu cortex-m3 -nographic \
    -kernel "$d/prog.elf" > "$d/console.log" 2>&1
check_out "$d/console.log" "the M3 image"
n=$(count_veneers "$d/prog.elf" .data thumbv7m-none-eabi "movw.*r12")
[ "$n" = 2 ] || fail "M3: .data has $n veneers, not 2 (flashfn twice is one, sflash)"
n=$(count_veneers "$d/prog.elf" .text thumbv7m-none-eabi "movw.*r12")
[ "$n" = 1 ] || fail "M3: .text has $n veneers, not 1 (main's two calls of ramfn)"
echo "Cortex-M3: RAM calls flash and flash calls RAM through movw/movt veneers"

# ---- 2. the same through a linker script ------------------------------------
cat > "$d/ram.ld" <<'EOF'
MEMORY { FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }
ENTRY(reset)
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
  .data : { __data_start = .; *(.ramfunc) *(.data*) __data_end = .; } > RAM AT > FLASH
  __data_load = LOADADDR(.data);
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; } > RAM
  .init_array : { __init_array_start = .; KEEP(*(.init_array*)) __init_array_end = .; } > RAM
}
EOF
"$EMBCC" --target=$T -O2 -fcommon '-DRAMSEC=".ramfunc"' -c "$out/prog.c" -o "$d/progs.o" ||
    fail "prog.c with .ramfunc"
"$EMBLD" -T "$d/ram.ld" "$d/boot.o" "$d/io.o" "$d/progs.o" -o "$d/progs.elf" \
    > "$d/links.log" 2>&1 || fail "the scripted image does not link: $(head -3 "$d/links.log")"
sh tests/harness/qrun.sh 10 "$QEMU" -M lm3s6965evb -cpu cortex-m3 -nographic \
    -kernel "$d/progs.elf" > "$d/consoles.log" 2>&1
check_out "$d/consoles.log" "the scripted M3 image"
echo "and through a linker script's .ramfunc"

# ---- 3. Cortex-M0 ---------------------------------------------------------------
T=thumbv6m-none-eabi
d=$out/m0
mkdir -p "$d"
if [ -f build/libc/$T/librt.a ]; then
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c tests/harness/thumb-m0/$f.c -o "$d/$f.o" || fail "$f.c"
    done
    "$EMBCC" --target=$T -O2 -fcommon -c "$out/prog.c" -o "$d/prog.o" || fail "prog.c for $T"
    EMBCC_THUMB_M0_HARNESS=$d sh tests/harness/thumb-m0/link.sh "$d/prog.elf" \
        "$d/prog.o" build/libc/$T/librt.a > "$d/link.log" 2>&1 ||
        fail "the M0 image does not link: $(head -3 "$d/link.log")"
    sh tests/harness/thumb-m0/run.sh "$d/prog.elf" > "$d/console.log" 2>&1
    check_out "$d/console.log" "the M0 image"
    "$OBJDUMP" -d --triple=$T "$d/prog.elf" | grep -q "movw" &&
        fail "M0: a movw, which ARMv6-M does not have"
    n=$(count_veneers "$d/prog.elf" .data $T "pop.*{r0, pc}")
    [ "$n" = 2 ] || fail "M0: .data has $n push/pop veneers, not 2"
    echo "Cortex-M0: push/ldr/str/pop veneers"
else
    echo "SKIP: the M0 half (build/libc/$T/librt.a is missing)"
fi

# ---- 4. ARMv7-A, ARM code ---------------------------------------------------------
T=armv7a-none-eabi
d=$out/a32
mkdir -p "$d"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c tests/harness/arm-a32/$f.c -o "$d/$f.o" || fail "$f.c"
done
"$EMBCC" --target=$T -O2 -fcommon -c "$out/prog.c" -o "$d/prog.o" || fail "prog.c for $T"
"$EMBLD" -e _start -Ttext 0x40100000 -Tdata 0x47000000 -Tstack 0x40800000 \
    "$d/boot.o" "$d/io.o" "$d/prog.o" -o "$d/prog.elf" > "$d/link.log" 2>&1 ||
    fail "the A32 image does not link: $(head -3 "$d/link.log")"
sh tests/harness/arm-a32/run.sh "$d/prog.elf" > "$d/console.log" 2>&1
check_out "$d/console.log" "the A32 image"
n=$("$OBJDUMP" -D --triple=$T "$d/prog.elf" | grep -c "ldr.*r12, \[pc\]")
[ "$n" = 3 ] || fail "A32: $n ldr ip, [pc] veneers, not 3 (two in .data, one in .text)"
echo "ARMv7-A: ldr/bx veneers, 112 MB both ways"

# ---- 5. interworking: a Thumb jump to an ARM function ----------------------------
CLANG=${EMBCC_REF_CLANG_ARM:-clang}
if command -v "$CLANG" >/dev/null 2>&1; then
    printf 'int armfn(int);\nint tjump(int x) { return armfn(x + 1); }\n' > "$d/tj.c"
    cat > "$d/main.c" <<'EOF'
void putn(long v);
void puts_(const char *s);
int tjump(int x);
int armfn(int x) { return x * 5; }
int main(void) { puts_("tj "); putn(tjump(7)); puts_("\n"); return 42; }
EOF
    "$CLANG" --target=$T -mthumb -O2 -c "$d/tj.c" -o "$d/tj.o" || fail "clang tj.c"
    "$OBJDUMP" -dr "$d/tj.o" | grep -q "R_ARM_THM_JUMP24.*armfn" ||
        fail "clang's tj.o has no Thumb jump to armfn: the case is not tested"
    "$EMBCC" --target=$T -O2 -c "$d/main.c" -o "$d/main.o" || fail "main.c"
    "$EMBLD" -e _start -Ttext 0x40100000 -Tstack 0x40800000 "$d/boot.o" \
        "$d/io.o" "$d/main.o" "$d/tj.o" -o "$d/tj.elf" > "$d/tj.log" 2>&1 ||
        fail "a Thumb jump to ARM code does not link: $(head -3 "$d/tj.log")"
    sh tests/harness/arm-a32/run.sh "$d/tj.elf" > "$d/tj.out" 2>&1
    tr -d '\r' < "$d/tj.out" | grep -q "^tj 40" ||
        fail "the interworking image did not print tj 40: $(head -4 "$d/tj.out")"
    echo "and a Thumb jump to ARM code switches state through its veneer"
else
    echo "SKIP: the interworking half (no $CLANG)"
fi
echo "ok embld-veneers"
