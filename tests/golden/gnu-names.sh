#!/bin/sh
# EmbCC under a GCC cross toolchain's names, so a project written for
# arm-none-eabi-gcc builds with EmbCC with only PATH changed.
#
#   <triple>-gcc / -cc   embcc, which reads the target from its own name, as
#                        clang does (and skips a version: -gcc-13.2);
#                        riscv64-unknown-elf-gcc -march=rv32... is RV32, as
#                        gcc's multilib driver makes it
#   -ar / -ranlib        embar: q, r, t, x, d, s, and ranlib by name
#   -objcopy             embpack, reading objcopy's command line for the
#                        image formats: -O binary|ihex|srec, -R, -j,
#                        --gap-fill, --pad-to
#   -size / -nm          embmap, printing binutils' tables
#
# The referees are llvm-objcopy, llvm-size and llvm-nm: the same command
# lines must give the same bytes and the same tables. Then a CMake project
# is built with the toolchain file EmbCC ships (cmake/embcc.cmake), and
# with a toolchain file written for arm-none-eabi-gcc -- the latter only
# with PATH pointing at the links.
set -u
echo "TEST-MARKER gnu-names"
. "$(dirname "$0")/../lib.sh"

for t in llvm-objcopy llvm-size llvm-nm llvm-readelf; do
    command -v $t > /dev/null 2>&1 || { echo "SKIP: no $t"; exit 0; }
done
EMBCC=${EMBCC:-./embcc}
ROOT=$(cd "$(dirname "$EMBCC")" && pwd)
for t in embar embpack embmap; do
    [ -x "$ROOT/$t" ] || { echo "SKIP: $ROOT/$t not built (make $t)"; exit 0; }
done
out=$PWD/tests/golden/out/gnu-names
rm -rf "$out"; mkdir -p "$out/bin"
fail() { echo "FAIL: $*"; exit 1; }

B=$out/bin
for tr in arm-none-eabi riscv64-unknown-elf avr; do
    ln -s "$ROOT/embcc" "$B/$tr-gcc"
    ln -s "$ROOT/embar" "$B/$tr-ar"
    ln -s "$ROOT/embar" "$B/$tr-ranlib"
    ln -s "$ROOT/embpack" "$B/$tr-objcopy"
    ln -s "$ROOT/embmap" "$B/$tr-size"
    ln -s "$ROOT/embmap" "$B/$tr-nm"
done
ln -s "$ROOT/embcc" "$B/arm-none-eabi-gcc-13.2"
ln -s "$ROOT/embcc" "$B/bogus-thing-gcc"
machine() { llvm-readelf -h "$1" | awk -F: '/Machine:/ { gsub(/^ +/, "", $2); print $2 }'; }
class() { llvm-readelf -h "$1" | awk -F: '/Class:/ { gsub(/^ +/, "", $2); print $2 }'; }

# ---- the compiler's names ---------------------------------------------------
printf 'int g = 3;\nstatic int s;\nconst int ro[4] = { 1, 2, 3, 4 };\nint f(int x) { s += x; return x * g + ro[x & 3]; }\n' > "$out/f.c"
"$B/arm-none-eabi-gcc" -mcpu=cortex-m4 -mthumb -Os -c "$out/f.c" -o "$out/arm.o" ||
    fail "arm-none-eabi-gcc -c"
[ "$(machine "$out/arm.o")" = ARM ] || fail "arm-none-eabi-gcc made $(machine "$out/arm.o")"
"$B/arm-none-eabi-gcc-13.2" -c "$out/f.c" -o "$out/arm2.o" || fail "arm-none-eabi-gcc-13.2"
[ "$(machine "$out/arm2.o")" = ARM ] || fail "the versioned name made $(machine "$out/arm2.o")"
"$B/riscv64-unknown-elf-gcc" -march=rv32imac_zicsr_zifencei -mabi=ilp32 -c "$out/f.c" \
    -o "$out/rv32.o" || fail "riscv64-unknown-elf-gcc -march=rv32"
[ "$(class "$out/rv32.o")" = ELF32 ] || fail "-march=rv32 under riscv64-unknown-elf made $(class "$out/rv32.o")"
"$B/riscv64-unknown-elf-gcc" -c "$out/f.c" -o "$out/rv64.o" || fail "riscv64-unknown-elf-gcc"
[ "$(class "$out/rv64.o")" = ELF64 ] || fail "riscv64-unknown-elf-gcc made $(class "$out/rv64.o")"
"$B/avr-gcc" -Os -c "$out/f.c" -o "$out/avr.o" || fail "avr-gcc"
machine "$out/avr.o" | grep -q AVR || fail "avr-gcc made $(machine "$out/avr.o")"
"$B/arm-none-eabi-gcc" --target=riscv32-unknown-elf -c "$out/f.c" -o "$out/over.o" ||
    fail "--target under a gcc name"
[ "$(machine "$out/over.o")" = RISC-V ] || fail "--target did not win over the name"
if "$B/bogus-thing-gcc" -c "$out/f.c" -o "$out/x.o" 2> "$out/bogus.err"; then
    fail "an unknown triple in the name was accepted"
fi
grep -q "called as 'bogus-thing-gcc', and 'bogus-thing' is not a target" "$out/bogus.err" ||
    { cat "$out/bogus.err"; fail "the unknown name was refused without saying why"; }
echo "gnu-names: <triple>-gcc compiles for the triple (version skipped, rv32 by -march, --target wins, unknown refused)"

# ---- objcopy ---------------------------------------------------------------
# An image with a gap between two flash sections, and a section far away
# that a build removes (-R), as AVR's .eeprom is.
cat > "$out/img.c" << 'EOF'
const unsigned char tab[40] __attribute__((section(".tab"))) = { 1, 2, 3, 4, 5, 6, 7 };
const unsigned char far[8] __attribute__((section(".far"))) = { 9, 9, 9 };
int counter = 5;
int bump(void) { return counter += tab[2] + far[1]; }
void _start(void) { for (;;) bump(); }
EOF
cat > "$out/img.ld" << 'EOF'
ENTRY(_start)
MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
         FAR (r) : ORIGIN = 0x08100000, LENGTH = 1K }
SECTIONS {
  .text : { *(.text*) *(.rodata*) } > FLASH
  .tab ALIGN(256) : { *(.tab) } > FLASH
  .data : { *(.data*) } > RAM AT > FLASH
  .bss : { *(.bss*) *(COMMON) } > RAM
  .far : { *(.far) } > FAR
}
EOF
# (binutils' permission warnings are switched off as SiFive's and xPack's
# RISC-V toolchain files do; they say nothing about the image)
"$B/arm-none-eabi-gcc" -mcpu=cortex-m4 -mthumb -Os -nostdlib "-T$out/img.ld" "$out/img.c" \
    -Wl,--no-warn-rwx-segments -Wl,--no-warn-execstack \
    -o "$out/img.elf" || fail "linking img.elf"
oc() {          # oc TAG ARGS...: ours and llvm's must write the same file
    tag=$1; shift
    "$B/arm-none-eabi-objcopy" "$@" "$out/img.elf" "$out/$tag.ours" 2> "$out/$tag.err" ||
        { cat "$out/$tag.err"; fail "objcopy $*"; }
    llvm-objcopy "$@" "$out/img.elf" "$out/$tag.llvm" || fail "llvm-objcopy $*"
    cmp -s "$out/$tag.ours" "$out/$tag.llvm" || fail "objcopy $* differs from llvm-objcopy"
}
oc hex -O ihex -R .far
oc bin -O binary -R .far
oc fill -O binary --gap-fill 0xff -R .far
oc pad -O binary -R .far --pad-to 0x08000800
oc only -O binary -j .tab
oc onlyhex -O ihex -j .text -j .data
oc farhex -O ihex
# S-records: the S0 header holds the output's name, so both are img.srec
mkdir -p "$out/s1" "$out/s2"
"$B/arm-none-eabi-objcopy" -O srec -R .far "$out/img.elf" "$out/s1/img.srec" || fail "objcopy -O srec"
(cd "$out/s2" && llvm-objcopy -O srec -R .far ../img.elf img.srec) || fail "llvm-objcopy -O srec"
(cd "$out/s1" && "$B/arm-none-eabi-objcopy" -O srec -R .far ../img.elf img.srec) || fail "objcopy -O srec"
cmp -s "$out/s1/img.srec" "$out/s2/img.srec" || fail "objcopy -O srec differs from llvm-objcopy"
if "$B/arm-none-eabi-objcopy" "$out/img.elf" "$out/copy.elf" 2> "$out/elf.err"; then
    fail "an ELF-to-ELF copy was accepted"
fi
grep -q "ELF-to-ELF copy" "$out/elf.err" || fail "the ELF-to-ELF refusal does not say so"
echo "gnu-names: objcopy -O ihex/srec/binary with -R, -j, --gap-fill and --pad-to writes llvm-objcopy's bytes"

# ---- size and nm -------------------------------------------------------------
squeeze() { sed 's/[ 	][ 	]*/ /g; s/^ //; s/ $//' "$1"; }
for f in img.elf arm.o; do
    "$B/arm-none-eabi-size" "$out/$f" > "$out/size.ours" || fail "size $f"
    llvm-size "$out/$f" > "$out/size.llvm"
    [ "$(squeeze "$out/size.ours")" = "$(squeeze "$out/size.llvm")" ] ||
        { diff "$out/size.llvm" "$out/size.ours"; fail "size $f differs from llvm-size"; }
    "$B/arm-none-eabi-size" --format=berkeley -t "$out/$f" "$out/$f" > "$out/size2.ours" ||
        fail "size -t"
    llvm-size --format=berkeley -t "$out/$f" "$out/$f" > "$out/size2.llvm"
    [ "$(squeeze "$out/size2.ours")" = "$(squeeze "$out/size2.llvm")" ] ||
        { diff "$out/size2.llvm" "$out/size2.ours"; fail "size -t $f differs from llvm-size"; }
    for o in "" "-S" "-g" "-u" "-n" "--defined-only"; do
        "$B/arm-none-eabi-nm" $o "$out/$f" > "$out/nm.ours" || fail "nm $o $f"
        llvm-nm $o "$out/$f" | grep -v ' [a-zA-Z] \$[adtx]' > "$out/nm.llvm"
        cmp -s "$out/nm.ours" "$out/nm.llvm" ||
            { diff "$out/nm.llvm" "$out/nm.ours"; fail "nm $o $f differs from llvm-nm"; }
    done
done
"$B/riscv64-unknown-elf-nm" -S "$out/rv64.o" > "$out/nm.ours" || fail "nm rv64.o"
llvm-nm -S "$out/rv64.o" > "$out/nm.llvm"
cmp -s "$out/nm.ours" "$out/nm.llvm" || { diff "$out/nm.llvm" "$out/nm.ours"; fail "nm on ELF64 differs"; }
echo "gnu-names: size (Berkeley, with totals) and nm (-S, -g, -u, -n, --defined-only) print llvm's tables, ELF32 and ELF64"

# ---- ar and ranlib ---------------------------------------------------------
cd "$out" || exit 1
"$B/arm-none-eabi-ar" qc lib1.a arm.o arm2.o || fail "ar qc"
"$B/arm-none-eabi-ranlib" lib1.a || fail "ranlib"
"$ROOT/embar" rcs lib2.a arm.o arm2.o || fail "embar rcs"
cmp -s lib1.a lib2.a || fail "ar qc + ranlib is not ar rcs"
llvm-nm --print-armap lib1.a | grep -q '^f in arm.o' || fail "the index ranlib wrote does not name f"
[ "$("$B/arm-none-eabi-ar" t lib1.a | tr '\n' ' ')" = "arm.o arm2.o " ] || fail "ar t"
mkdir x && (cd x && "$B/arm-none-eabi-ar" x ../lib1.a arm2.o) || fail "ar x"
cmp -s x/arm2.o arm2.o || fail "ar x wrote another member's bytes"
[ ! -f x/arm.o ] || fail "ar x MEMBER extracted more than the member"
"$B/arm-none-eabi-ar" d lib1.a arm.o || fail "ar d"
[ "$("$B/arm-none-eabi-ar" t lib1.a)" = "arm2.o" ] || fail "ar d left $("$B/arm-none-eabi-ar" t lib1.a)"
if "$B/arm-none-eabi-ranlib" nosuch.a 2> /dev/null; then fail "ranlib of no archive succeeded"; fi
cd - > /dev/null || exit 1
echo "gnu-names: ar qc + ranlib writes ar rcs's archive; t, x, d; ranlib refuses a missing archive"

# ---- CMake -------------------------------------------------------------------
command -v cmake > /dev/null 2>&1 || { echo "gnu-names: cmake part skipped: no cmake"; exit 0; }
P=$out/proj
mkdir -p "$P/src" "$out/tree/cmake"
cat > "$P/CMakeLists.txt" << 'EOF'
cmake_minimum_required(VERSION 3.20)
project(gn LANGUAGES C ASM)
add_library(util STATIC src/util.c)
add_executable(app src/main.c)
target_link_libraries(app PRIVATE util)
target_link_options(app PRIVATE -nostdlib -T${CMAKE_SOURCE_DIR}/img.ld)
add_custom_command(TARGET app POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex -R .far $<TARGET_FILE:app> app.hex
  COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:app>)
EOF
cp "$out/img.ld" "$P/img.ld"
printf '#define SCALE 3\n' > "$P/src/util.h"
printf '#include "util.h"\nint util(int x) { return x * SCALE; }\n' > "$P/src/util.c"
printf 'int util(int);\nvolatile int sink;\nvoid _start(void) { for (;;) sink = util(sink); }\n' > "$P/src/main.c"
gen=; command -v ninja > /dev/null 2>&1 && gen="-G Ninja"
# a source-tree layout, as cmake/embcc.cmake expects to find its tools
cp "$ROOT/cmake/embcc.cmake" "$out/tree/cmake/"
for t in embcc embar embpack embmap; do ln -s "$ROOT/$t" "$out/tree/$t"; done
ln -s embar "$out/tree/embcc-ranlib"; ln -s embpack "$out/tree/embcc-objcopy"
ln -s embmap "$out/tree/embcc-size"; ln -s embmap "$out/tree/embcc-nm"
cmake -S "$P" -B "$out/b1" $gen -DCMAKE_TOOLCHAIN_FILE="$out/tree/cmake/embcc.cmake" \
    -DEMBCC_TARGET=thumbv7em-none-eabi "-DEMBCC_FLAGS=-mcpu=cortex-m4 -Os" \
    > "$out/b1.cfg" 2>&1 || { tail -20 "$out/b1.cfg"; fail "configure with cmake/embcc.cmake"; }
cmake --build "$out/b1" > "$out/b1.log" 2>&1 || { tail -20 "$out/b1.log"; fail "build with cmake/embcc.cmake"; }
[ -s "$out/b1/app.hex" ] || fail "no app.hex from the post-build objcopy"
grep -q 'text.*data.*bss' "$out/b1.log" || fail "no size table from the post-build size"
[ "$(machine "$out/b1/app")" = ARM ] || fail "the CMake build made $(machine "$out/b1/app")"
# a header edit rebuilds what includes it (the -MD depfiles)
sleep 1
printf '#define SCALE 5\n' > "$P/src/util.h"
cmake --build "$out/b1" > "$out/b1b.log" 2>&1 || fail "rebuild"
grep -q 'util.c' "$out/b1b.log" || { cat "$out/b1b.log"; fail "editing util.h did not rebuild util.c"; }
# the same project with a toolchain file written for GCC, which names its
# archiver as many do (CMake finds <prefix>-ar by itself only for a compiler
# it identifies as GCC or Clang, and it does not identify EmbCC): PATH alone
cat > "$out/gcc-tc.cmake" << 'EOF'
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR arm-none-eabi-ar)
set(CMAKE_RANLIB arm-none-eabi-ranlib)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_SIZE arm-none-eabi-size)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m4 -mthumb -Os")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")
EOF
PATH="$B:$PATH" cmake -S "$P" -B "$out/b2" $gen -DCMAKE_TOOLCHAIN_FILE="$out/gcc-tc.cmake" \
    > "$out/b2.cfg" 2>&1 || { tail -20 "$out/b2.cfg"; fail "configure with a gcc toolchain file"; }
PATH="$B:$PATH" cmake --build "$out/b2" > "$out/b2.log" 2>&1 ||
    { tail -20 "$out/b2.log"; fail "build with a gcc toolchain file"; }
[ -s "$out/b2/app.hex" ] || fail "no app.hex from the gcc-named build"
cmp -s "$out/b1/app.hex" "$out/b2/app.hex" ||
    echo "note: the two CMake builds' images differ (flags differ: -mthumb, the header edit)"
echo "gnu-names: a CMake project builds with cmake/embcc.cmake (hex, size, header dependencies) and with a GCC toolchain file through the links"
