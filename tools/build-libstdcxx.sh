#!/bin/sh
# CX8's second half: libstdc++'s own sources compiled by EmbCC.
#
# Every object the reference g++'s build of libstdc++ made (the configured
# tree tools/build-ref-gxx.sh leaves) is compiled again by EmbCC — from the
# same source, with the standard its directory is built with (-std=gnu++98
# for src/c++98, ...), the same include paths and -D flags — into $OUT.
# What fails is listed with its first error; the tally closes the run.
#
#   usage: tools/build-libstdcxx.sh [x86_64-elf|aarch64-elf] [dir...]
#          dirs: libsupc++ src/c++98 src/c++11 src/c++17 src/c++20
#                src/c++23 src/c++26 (default: all)
set -u
target=${1:-x86_64-elf}
[ $# -gt 0 ] && shift
root=$(cd "$(dirname "$0")/.." && pwd)
. "$root/tools/hostpaths.sh"
case $target in
aarch64-elf) NL=$AARCH64_NEWLIB ;;
*) NL=$X86_NEWLIB ;;
esac
SRC=${GCC_SRC:-$HOME/cross/src/gcc-16.2.0}/libstdc++-v3
B=${LIBSTDCXX_BUILD:-$HOME/cross/src/build-gcc-cxx-$target/$target/libstdc++-v3}
OUT=${OUT:-$root/build/libstdcxx-$target}
EMBCC=${EMBCC:-$root/embcc}
[ -d "$B" ] || { echo "no configured libstdc++ build at $B" >&2; exit 1; }

dirs=${*:-"libsupc++ src/c++98 src/c++11 src/c++17 src/c++20 src/c++23 src/c++26"}
ok=0 bad=0
fails=$OUT/failures.txt
mkdir -p "$OUT"
: > "$fails"
for d in $dirs; do
    case $d in
    libsupc++) std=gnu++20 ;;
    src/c++*) std=gnu++${d#src/c++} ;;
    *) echo "unknown directory $d" >&2; exit 1 ;;
    esac
    mkdir -p "$OUT/$d"
    for o in "$B/$d"/*.o; do
        [ -e "$o" ] || continue
        name=$(basename "$o" .o)
        extra=
        case "$d/$name" in
        src/c++11/codecvt|src/c++11/limits|src/c++11/locale_init|\
        src/c++11/localename)
            extra=-fchar8_t ;;
        esac
        src=
        for cand in "$B/$d/$name.cc" "$SRC/$d/$name.cc" "$B/$d/$name.c" \
                    "$SRC/$d/$name.c"; do
            if [ -f "$cand" ]; then src=$cand; break; fi
        done
        if [ -z "$src" ]; then
            echo "$d/$name: no source found" >> "$fails"
            bad=$((bad + 1))
            continue
        fi
        case $src in
        *.c) lang="-x c -DIN_GLIBCPP_V3 -DHAVE_ALLOCA_H -I$SRC/../include" std_flag= ;;
                                                        # cp-demangle.c
        *) lang= std_flag=-std=$std ;;
        esac
        rm -f "$OUT/$d/$name.o"
        if msg=$("$EMBCC" --target="$target" $lang $std_flag $extra \
                     -DHAVE_CONFIG_H -I"$B/$d" -I"$SRC/$d" -I"$B" \
                     -I"$B/include/$target" -I"$B/include" \
                     -I"$SRC/libsupc++" -I"$SRC/../libgcc" \
                     -I"$SRC/../libiberty" -I"$NL/include" \
                     -c "$src" -o "$OUT/$d/$name.o" 2>&1); then
            ok=$((ok + 1))
        else
            bad=$((bad + 1))
            first=$(printf '%s\n' "$msg" | grep -m1 'error' || printf '%s\n' "$msg" | head -1)
            echo "$d/$name: $first" >> "$fails"
        fi
    done
done
cat "$fails"
echo "libstdc++ for $target: $ok compiled, $bad failed (objects in $OUT)"

# The archives: the reference build's libstdc++.a and libsupc++.a with each
# member EmbCC compiled put in its place — $OUT/ref stands in for
# EMBCC_REF_GXX when linking (tests/harness/*/link.sh --cxx).
REF=$( [ "$target" = aarch64-elf ] && echo "$AARCH64_REF_GXX" || echo "$X86_REF_GXX" )
AR=${AR:-$target-ar}
lib=$OUT/ref/$target/lib
mkdir -p "$lib"
cp "$REF/$target/lib/libstdc++.a" "$REF/$target/lib/libsupc++.a" "$lib/"
for obj in "$OUT"/libsupc++/*.o "$OUT"/src/*/*.o; do
    [ -e "$obj" ] || continue
    "$AR" r "$lib/libstdc++.a" "$obj"
    case $obj in
    "$OUT"/libsupc++/*) "$AR" r "$lib/libsupc++.a" "$obj" ;;
    esac
done
echo "archives with EmbCC's objects: $lib"
[ "$bad" -eq 0 ]
