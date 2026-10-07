#!/bin/sh
# EmbCC's C++ links with clang++'s on the 32-bit embedded targets: the
# C++ ABI there is the Itanium ABI's 32-bit form, with the ARM C++ ABI's
# changes on Cortex-M. tests/golden/cxx-abi-ilp32/side_a.cc and side_b.cc
# call each other through abi.h, and every pairing -- EmbCC/EmbCC,
# EmbCC/clang++, clang++/EmbCC, clang++/clang++ -- is linked by embld,
# run on the board and must print what the two sides print built for the
# host. Between them they cross:
#
#   virtual calls on objects the other side built, a deleting destructor
#     through a base, this-adjusting thunks (two interfaces on one
#     object), a virtual base (vbase offsets, VTTs);
#   mangled names: size_t, ptrdiff_t, long, wchar_t, char16_t and every
#     other builtin type, references, arrays by reference, explicit
#     template instantiations, operators, function pointers;
#   member-function pointers (virtual ones, null ones, converted to a
#     derived class whose base is not at offset 0, compared) and a pointer
#     to a data member;
#   static initialization: objects with constructors in each unit
#     (.init_array), a guarded local static, and an inline function's
#     static that BOTH units define, so one guard serves the two;
#   new[] in one unit and delete[] in the other (the array cookie);
#   with RTTI (a second pass, -frtti): dynamic_cast down, across and
#     through a virtual base, and typeid, on classes whose type_info the
#     other compiler emitted, through the runtime's __dynamic_cast;
#   a class with a copy constructor passed and returned by value, a small
#     trivial one by value, and constructors called through new from the
#     other side (on ARM a constructor returns `this`).
#
# Boards: a Cortex-M3 (thumbv7m), a Cortex-M4F with the hard-float
# convention (thumbv7em-none-eabihf) and RV32 (riscv32-unknown-elf).
set -u
echo "TEST-MARKER cxx-abi-ilp32"
. "$(dirname "$0")/../lib.sh"

CLANGXX=${EMBCC_CLANGXX:-clang++}
NM=${EMBCC_LLVM_NM:-llvm-nm}
command -v "$CLANGXX" >/dev/null 2>&1 || {
    echo "skipped: no $CLANGXX (EMBCC_CLANGXX)"; exit 0; }
command -v qemu-system-arm >/dev/null 2>&1 &&
    command -v qemu-system-riscv32 >/dev/null 2>&1 || {
    echo "skipped: qemu-system-arm and qemu-system-riscv32 are needed"
    exit 0; }

cd "$EMBCC_ROOT"
D=tests/golden/cxx-abi-ilp32
out=tests/golden/out/cxx-abi-ilp32
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-$PWD/embcc}
EMBLD=${EMBLD:-$PWD/embld}
export EMBCC EMBLD

# ---- the reference: both sides for the host -------------------------------
"$CLANGXX" -std=c++20 -w -o "$out/host" "$D/side_a.cc" "$D/side_b.cc" || {
    echo "the host reference does not build"; exit 1; }
"$out/host" > "$out/host.txt" 2>&1
grep -q '==END==' "$out/host.txt" || {
    echo "the host reference does not reach ==END=="; exit 1; }
# ... and for the RTTI pass (-frtti -DABI_RTTI: typeid and dynamic_cast
# across the two units)
"$CLANGXX" -std=c++20 -w -DABI_RTTI -o "$out/host-rtti" "$D/side_a.cc" \
    "$D/side_b.cc" || { echo "the host RTTI reference does not build"; exit 1; }
"$out/host-rtti" > "$out/host-rtti.txt" 2>&1
grep -q '^rtti ' "$out/host-rtti.txt" || {
    echo "the host RTTI reference prints no rtti line"; exit 1; }

fail=0
for t in thumbv7m-none-eabi thumbv7em-none-eabihf riscv32-unknown-elf; do
    d=$out/$t; mkdir -p "$d"
    case $t in
        thumbv7m*)
            H=tests/harness/thumb; hv=EMBCC_THUMB_HARNESS
            CL="--target=$t -mcpu=cortex-m3 -mfloat-abi=soft"
            set -- qemu-system-arm -M lm3s6965evb -cpu cortex-m3 ;;
        thumbv7em*)
            H=tests/harness/thumb-m4f; hv=EMBCC_THUMB_HARNESS
            CL="--target=$t -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
            set -- qemu-system-arm -M mps2-an386 -cpu cortex-m4 ;;
        riscv32*)
            H=tests/harness/riscv; hv=EMBCC_RISCV_HARNESS
            CL="--target=$t -march=rv32imac -mabi=ilp32 -mno-relax"
            set -- qemu-system-riscv32 -M virt -bios none -m 8 ;;
    esac
    { sh tools/build-rt.sh "$t" "$d" && sh tools/build-libc.sh "$t" "$d" &&
      sh tools/build-libcxx.sh "$t" "$d"; } > "$d/build.log" 2>&1 || {
        echo "$t: the libraries do not build:"; tail -3 "$d/build.log"
        exit 1; }
    for f in boot io; do
        "$EMBCC" --target="$t" -c "$H/$f.c" -o "$d/$f.o" || exit 1
    done
    "$EMBCC" --target="$t" -c tests/cxx-embedded/board.c -o "$d/board.o" ||
        exit 1
    # clang++ over EmbCC's C library headers, as a firmware build would be
    CL="$CL -std=c++20 -fno-exceptions -nostdlibinc
        -isystem lib/libcxx/include -isystem lib/libc/include -w -c"
    n=0
    # without RTTI at every level, then with it (rt: -frtti -DABI_RTTI)
    for combo in e:e e:c c:e c:c rt:e:e rt:e:c rt:c:e; do
        mode=; ref=$out/host.txt; rtti=-fno-rtti
        case $combo in rt:*) mode=r; ref=$out/host-rtti.txt
                             rtti="-frtti -DABI_RTTI"; combo=${combo#rt:} ;;
        esac
        sa=${combo%:*} sb=${combo#*:}
        for opt in -O0 -O2 -Os; do
            [ "$combo" = c:c ] && [ "$opt" != -O2 ] && continue
            [ -n "$mode" ] && [ "$opt" = -Os ] && continue
            tag=$mode$sa$sb$opt
            for side in a b; do
                if [ "$side" = a ]; then who=$sa; else who=$sb; fi
                rm -f "$d/$side.$tag.o"
                if [ "$who" = e ]; then
                    "$EMBCC" --target="$t" $opt -fno-exceptions $rtti \
                        -Ilib/libc/include -c "$D/side_$side.cc" \
                        -o "$d/$side.$tag.o"
                else
                    # clang at -O2 against EmbCC's levels; -O0 with EmbCC -O0
                    co=-O2; [ "$opt" = -O0 ] && co=-O0
                    $CLANGXX $CL $rtti $co "$D/side_$side.cc" \
                        -o "$d/$side.$tag.o"
                fi > "$d/$tag.err" 2>&1 || {
                    echo "$t $tag: side $side ($who) does not compile:"
                    head -3 "$d/$tag.err"; fail=1; continue 2; }
            done
            rm -f "$d/$tag.elf"
            env "$hv=$d" sh "$H/link.sh" "$d/$tag.elf" "$d/a.$tag.o" \
                "$d/b.$tag.o" "$d/board.o" "$d/libcxx.a" "$d/libc.a" \
                "$d/librt.a" > "$d/$tag.err" 2>&1 || {
                echo "$t $tag: does not link:"; head -3 "$d/$tag.err"
                fail=1; continue; }
            sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" \
                --until '==END==' "$@" -nographic -kernel "$d/$tag.elf" \
                > "$d/$tag.raw" 2>/dev/null
            sed -n '1,/==END==/p' "$d/$tag.raw" > "$d/$tag.txt"
            if ! diff "$ref" "$d/$tag.txt" > "$d/$tag.diff"; then
                echo "$t $tag (side a: $sa, side b: $sb): differs from the host:"
                head -8 "$d/$tag.diff"; fail=1
            fi
            n=$((n + 1))
        done
    done
    # The data the ABI lays out, compared object to object: every vtable,
    # VTT, construction vtable and guard variable both compilers define for
    # a side must be the same size (a guard is 4 bytes on ARM, 8 on RV32).
    if command -v "$NM" >/dev/null 2>&1; then
        for pair in "a ee-O0 ce-O0" "b ee-O0 ec-O0"; do
            set -- $pair
            for who in "$2" "$3"; do
                "$NM" -S --defined-only "$d/$1.$who.o" | awk '
                    NF == 4 && $4 ~ /^_Z(TV|TT|TC|GV)/ { print $4, $2 }' |
                    sort > "$d/sym.$1.$who"
            done
            join "$d/sym.$1.$2" "$d/sym.$1.$3" |
                awk '$2 != $3 { print "  " $1 ": EmbCC " $2 " bytes, clang " $3 }' \
                > "$d/sym.$1.diff"
            k=$(join "$d/sym.$1.$2" "$d/sym.$1.$3" | wc -l)
            if [ -s "$d/sym.$1.diff" ] || [ "$k" -lt 4 ]; then
                echo "$t side $1: the ABI's data differ in size ($k compared):"
                cat "$d/sym.$1.diff"; fail=1
            fi
        done
    fi
    [ "$fail" = 0 ] && echo "$t: $n pairings of EmbCC and clang++ agree with the host"
done
[ "$fail" = 0 ] || exit 1
echo "EmbCC's C++ and clang++'s call each other on Cortex-M and RV32"
