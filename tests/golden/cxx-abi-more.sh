#!/bin/sh
# EmbCC's C++ links with clang++'s on MIPS32 (both byte orders), MIPS64
# big-endian, SPARC and PowerPC, and with g++'s on Xtensa (Espressif's GCC
# for the de212 core, tools/xtensa-ref-gcc.sh): the generic Itanium C++
# ABI there --
# MIPS with ARM's member-function pointers (clang's GenericMIPS: a virtual
# one flagged in adj) -- and on all of them but little-endian MIPS32 laid
# out big-endian. The same two units as tests/golden/cxx-abi-ilp32.sh
# (tests/golden/cxx-abi-ilp32/side_a.cc and side_b.cc, through abi.h) are
# compiled EmbCC/EmbCC, EmbCC/clang++, clang++/EmbCC and clang++/clang++,
# linked by embld with the embedded C++ runtime, run on each board under
# QEMU, and must print what the two sides print built for the host:
# virtual calls, thunks and virtual bases across the units, mangled names
# (size_t is `unsigned long` on bare-metal PowerPC: _Znwm), member
# pointers, guards, array cookies, static initialization, and -- in the
# -frtti pass -- dynamic_cast and typeid on type_info the other compiler
# wrote. The vtables, VTTs and guards both compilers define must also be
# the same size.
#
# The boards boot as their exec suites do (tests/harness/<board>): boot.c
# built with -DHARNESS_LIBC, io.c's weak write() to the UART, run.sh.
# ColdFire, TriCore and RX have no C++ compiler here to compare with: on
# them the two units are compiled by EmbCC alone, at every level and with
# RTTI, so at least the convention EmbCC chose agrees with itself across
# units (a class returned through its return slot in a1, in a4, in r15).
set -u
echo "TEST-MARKER cxx-abi-more"
. "$(dirname "$0")/../lib.sh"

CLANGXX=${EMBCC_CLANGXX:-clang++}
NM=${EMBCC_LLVM_NM:-llvm-nm}
command -v "$CLANGXX" >/dev/null 2>&1 || {
    echo "skipped: no $CLANGXX (EMBCC_CLANGXX)"; exit 0; }

cd "$EMBCC_ROOT"
D=tests/golden/cxx-abi-ilp32
out=tests/golden/out/cxx-abi-more
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
"$CLANGXX" -std=c++20 -w -DABI_RTTI -o "$out/host-rtti" "$D/side_a.cc" \
    "$D/side_b.cc" || { echo "the host RTTI reference does not build"; exit 1; }
"$out/host-rtti" > "$out/host-rtti.txt" 2>&1
grep -q '^rtti ' "$out/host-rtti.txt" || {
    echo "the host RTTI reference prints no rtti line"; exit 1; }

fail=0
boards=${EMBCC_CXX_ABI_BOARDS:-"mipsel-none-elf mips-none-elf mips64-none-elf
        sparc-none-elf powerpc-none-eabi xtensa-none-elf m68k-none-elf
        tricore-none-elf rx-none-elf"}
XGXX=${XTENSA_REF_GCC%gcc}g++
for t in $boards; do
    d=$out/$t; mkdir -p "$d"
    REF=$CLANGXX
    case $t in
        mipsel*)
            SRC=tests/harness/mips H=tests/harness/mips hv=EMBCC_MIPS_HARNESS
            CL="--target=mipsel-unknown-elf -mcpu=mips32r2 -msoft-float"
            Q=qemu-system-mipsel ;;
        mips64*)
            SRC=tests/harness/mips H=tests/harness/mips64
            hv=EMBCC_MIPS64_HARNESS
            CL="--target=mips64-unknown-elf -mcpu=mips64r2 -msoft-float
                -mno-abicalls -G0"
            Q=qemu-system-mips64 ;;
        mips*)
            SRC=tests/harness/mips H=tests/harness/mips hv=EMBCC_MIPS_HARNESS
            CL="--target=mips-unknown-elf -mcpu=mips32r2 -msoft-float"
            Q=qemu-system-mips ;;
        sparc*)
            SRC=tests/harness/sparc H=$SRC hv=EMBCC_SPARC_HARNESS
            CL="--target=sparc-none-elf -mcpu=leon3 -msoft-float"
            Q=qemu-system-sparc ;;
        powerpc*)
            SRC=tests/harness/ppc H=$SRC hv=EMBCC_PPC_HARNESS
            CL="--target=powerpc-none-eabi -mcpu=e500 -mno-spe -msoft-float
                -mlong-double-64"
            Q=qemu-system-ppc ;;
        xtensa*)
            SRC=tests/harness/xtensa H=$SRC hv=EMBCC_XTENSA_HARNESS
            REF=$XGXX CL="$XTENSA_REF_FLAGS -mtext-section-literals"
            Q=qemu-system-xtensa ;;
        m68k*)
            SRC=tests/harness/coldfire H=$SRC hv=EMBCC_CF_HARNESS
            REF= CL= Q=qemu-system-m68k ;;
        tricore*)
            SRC=tests/harness/tricore H=$SRC hv=EMBCC_TRICORE_HARNESS
            REF= CL= Q=qemu-system-tricore ;;
        rx*)
            SRC=tests/harness/rx H=$SRC hv=EMBCC_RX_HARNESS
            REF= CL= Q=qemu-system-rx ;;
    esac
    command -v $Q >/dev/null 2>&1 || {
        echo "$t: skipped, no $Q"; continue; }
    if [ -n "$REF" ] && ! "$REF" $CL -x c++ -fsyntax-only /dev/null \
            2>/dev/null; then
        echo "$t: no $REF for $t: EmbCC with itself only"; REF=
    fi
    case $t in tricore*)
        [ -n "${EMBCC_TRICORE_PLUGIN:-}" ] || {
            inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
            cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
               -undefined dynamic_lookup -o "$out/putc.so" \
               tests/harness/tricore/putc.c 2>/dev/null ||
            cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
               -o "$out/putc.so" tests/harness/tricore/putc.c || {
                echo "the TriCore harness's output plugin does not build"; exit 1; }
            EMBCC_TRICORE_PLUGIN=$PWD/$out/putc.so
            export EMBCC_TRICORE_PLUGIN; } ;;
    esac
    { sh tools/build-rt.sh "$t" "$d" && sh tools/build-libc.sh "$t" "$d" &&
      sh tools/build-libcxx.sh "$t" "$d"; } > "$d/build.log" 2>&1 || {
        echo "$t: the libraries do not build:"; tail -3 "$d/build.log"
        exit 1; }
    "$EMBCC" --target="$t" -O1 -DHARNESS_LIBC -c "$SRC/boot.c" \
        -o "$d/boot.o" || exit 1
    "$EMBCC" --target="$t" -O1 -c "$SRC/io.c" -o "$d/io.o" || exit 1
    # the reference compiler over EmbCC's C library headers, as a firmware
    # build would be (g++ has no -nostdlibinc: EmbCC's freestanding ones)
    case $REF in
        *g++) CL="$CL -std=c++20 -fno-exceptions -nostdinc
                  -isystem lib/libcxx/include -isystem lib/libc/include
                  -isystem include -w -c" ;;
        *)    CL="$CL -std=c++20 -fno-exceptions -nostdlibinc
                  -isystem lib/libcxx/include -isystem lib/libc/include -w -c" ;;
    esac
    refname=${REF:-EmbCC}; refname=${refname##*/}
    combos="e:e e:c c:e c:c rt:e:e rt:e:c rt:c:e"
    [ -n "$REF" ] || combos="e:e rt:e:e"
    n=0 known=0
    for combo in $combos; do
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
                    # shellcheck disable=SC2086
                    "$EMBCC" --target="$t" $opt -fno-exceptions $rtti \
                        -Ilib/libc/include -c "$D/side_$side.cc" \
                        -o "$d/$side.$tag.o"
                else
                    co=-O2; [ "$opt" = -O0 ] && co=-O0
                    # shellcheck disable=SC2086
                    "$REF" $CL $rtti $co "$D/side_$side.cc" \
                        -o "$d/$side.$tag.o"
                fi > "$d/$tag.err" 2>&1 || {
                    echo "$t $tag: side $side ($who) does not compile:"
                    head -3 "$d/$tag.err"; fail=1; continue 2; }
            done
            rm -f "$d/$tag.elf"
            env "$hv=$d" sh "$H/link.sh" "$d/$tag.elf" "$d/a.$tag.o" \
                "$d/b.$tag.o" "$d/libcxx.a" "$d/libc.a" \
                "$d/librt.a" > "$d/$tag.err" 2>&1 || {
                echo "$t $tag: does not link:"; head -3 "$d/$tag.err"
                fail=1; continue; }
            img=$d/$tag.elf
            case $t in rx*) img=$d/$tag.bin ;; esac
            EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-30} \
                sh "$H/run.sh" "$img" > "$d/$tag.raw" 2>/dev/null
            tr -d '\r' < "$d/$tag.raw" | sed -n '1,/==END==/p' > "$d/$tag.txt"
            if diff "$ref" "$d/$tag.txt" > "$d/$tag.diff"; then
                :
            elif [ "$t" = mips64-none-elf ] && [ "$sa" != "$sb" ] &&
                 [ "$(grep '^[<>]' "$d/$tag.diff" | grep -vc '^[<>] mix ')" = 0 ]
            then
                # n64 big-endian puts a float passed on the STACK in its
                # slot's first four bytes (GCC's mips_pad_arg_upward,
                # clang's CCAssignToStack<4, 8>); EmbCC's MIPS64 backend
                # puts it in the last four -- a C calling-convention
                # difference, which detail::mix's 13th argument crosses.
                # Every other line must agree.
                known=$((known + 1))
            else
                echo "$t $tag (side a: $sa, side b: $sb): differs from the host:"
                head -8 "$d/$tag.diff"; fail=1
            fi
            n=$((n + 1))
        done
    done
    # the data the ABI lays out, object to object: the same sizes
    if [ -n "$REF" ] && command -v "$NM" >/dev/null 2>&1; then
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
    [ "$known" = 0 ] ||
        echo "$t: $known mixed pairings differ in detail::mix only: a float on the stack (a C ABI difference)"
    [ "$fail" = 0 ] && [ "$known" = 0 ] &&
        echo "$t: $n pairings of EmbCC and $refname agree with the host"
    [ "$fail" = 0 ] && [ "$known" != 0 ] &&
        echo "$t: $n pairings of EmbCC and $refname, all but that line agree with the host"
done
[ "$fail" = 0 ] || exit 1
echo "EmbCC's C++ and clang++'s call each other on MIPS, SPARC and PowerPC, and"
echo "g++'s on Xtensa; and EmbCC's with itself on ColdFire, TriCore and RX"
