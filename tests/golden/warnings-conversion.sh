#!/bin/sh
# -Wconversion and its family, as GCC draws them (sema, cvt_check). The
# rules are GCC's because coding standards for embedded C are written
# against them, and because a project that builds -Werror-clean with GCC
# has to build with EmbCC: EmbLinkRTOS passes -Wconversion
# -Wsign-conversion -Werror. So the test is two-sided -- every warning
# GCC gives on warnings-conversion.c, and nothing GCC does not give, on
# that file and on lib/libc's 60-odd sources, which GCC is the referee
# for when x86_64-elf-gcc is installed.
set -u
echo "TEST-MARKER warnings-conversion"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
src=tests/golden/warnings-conversion.c
out=tests/golden/out/warnings-conversion
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# "LINE OPTION" for every warning of the family, sorted
pick() {
    grep 'warning:' | grep -E '\[-W(sign-|float-|arith-)?conversion\]|\[-Woverflow\]' |
        sed -E 's/^(embcc: )?[^:]*:([0-9]+):[0-9]+: warning: .*\[-W([a-z-]+)\]$/\2 \3/' | sort
}
# what the W: annotations promise, filtered to the options given
want() {
    awk -v keep="$1" '{
        if (match($0, /W:[a-z,-]+/)) {
            n = split(substr($0, RSTART + 2, RLENGTH - 2), w, ",")
            for (k = 1; k <= n; k++) if (keep ~ ("(^| )" w[k] "( |$)")) print NR, w[k]
        }
    }' "$src" | sort
}
run() { "$EMBCC" --target=x86_64-elf -fsyntax-only "$@" "$src" 2>&1 | pick; }
same() {    # NAME EXPECTED ACTUAL
    [ "$2" = "$3" ] && return 0
    echo "expected:"; echo "$2" > "$out/a"; echo "$3" > "$out/b"
    diff "$out/a" "$out/b" | grep '^[<>]' | sed 's/^</  missing/; s/^>/  extra  /'
    fail "$1"
}

ALL="conversion sign-conversion float-conversion overflow"
same "-Wconversion: the warnings at their lines" "$(want "$ALL")" "$(run -Wconversion)"
echo "warnings-conversion: -Wconversion: $(want "$ALL" | wc -l | tr -d ' ') warnings, each at its line, and none elsewhere"

# the grouping: -Wconversion brings -Wsign-conversion and
# -Wfloat-conversion unless either is named, before or after it
same "-Wsign-conversion alone" "$(want "sign-conversion overflow")" "$(run -Wsign-conversion)"
same "-Wfloat-conversion alone" "$(want "float-conversion overflow")" "$(run -Wfloat-conversion)"
same "-Wconversion -Wno-sign-conversion" "$(want "conversion float-conversion overflow")" \
    "$(run -Wconversion -Wno-sign-conversion)"
same "-Wno-sign-conversion -Wconversion" "$(want "conversion float-conversion overflow")" \
    "$(run -Wno-sign-conversion -Wconversion)"
same "no flags: -Woverflow alone, on by default" "$(want overflow)" "$(run)"
same "-Wno-overflow" "" "$(run -Wno-overflow)"
"$EMBCC" --target=x86_64-elf -fsyntax-only -Wconversion -Werror "$src" > /dev/null 2>&1 &&
    fail "-Werror let a conversion warning through"
echo "warnings-conversion: -Wsign-conversion and -Wfloat-conversion come with -Wconversion unless named; -Woverflow is on by default"

# -Warith-conversion: arithmetic is suspect even when its operands fit
printf 'typedef unsigned char u8;\nu8 f(u8 b) { return b + b; }\nu8 g(u8 b) { return b; }\n' > "$out/arith.c"
"$EMBCC" --target=x86_64-elf -fsyntax-only -Wconversion "$out/arith.c" 2>&1 | grep -q warning &&
    fail "u8 + u8 into a u8 warned without -Warith-conversion"
"$EMBCC" --target=x86_64-elf -fsyntax-only -Wconversion -Warith-conversion "$out/arith.c" 2>&1 |
    grep -q ':2:.*\[-Warith-conversion\]' || fail "-Warith-conversion said nothing about b + b"

# the widths are the target's: a long is an int on ILP32
printf 'int f(long l) { return l; }\nint g(long long q) { return q; }\n' > "$out/w.c"
"$EMBCC" --target=thumbv7em-none-eabi -fsyntax-only -Wconversion "$out/w.c" 2>&1 | pick > "$out/w"
[ "$(cat "$out/w")" = "2 conversion" ] || { cat "$out/w"; fail "Cortex-M: long to int is no narrowing, long long is"; }

echo "warnings-conversion: -Warith-conversion, and the target's widths"

GCC=x86_64-elf-gcc
command -v $GCC > /dev/null 2>&1 || { echo "SKIP: no $GCC to referee against"; exit 0; }
# GCC agrees, line for line -- and warns on each gcc-folds line, which is
# what makes leaving them out a choice rather than a gap
$GCC -fsyntax-only -ffreestanding -Wconversion "$src" 2>&1 | pick > "$out/g"
want "$ALL" > "$out/w"
awk '/gcc-folds/ { print NR }' "$src" > "$out/folds"
grep -v -w -F -f "$out/folds" "$out/g" > "$out/g2"
same "GCC disagrees with the annotations" "$(cat "$out/w")" "$(cat "$out/g2")"
[ "$(wc -l < "$out/g")" -gt "$(wc -l < "$out/g2")" ] || fail "GCC no longer warns on the gcc-folds lines"
echo "warnings-conversion: GCC 16 gives the same warnings at the same lines"

# and nothing GCC would not say, over lib/libc
F="-ffreestanding -nostdinc -Ilib/libc/include -isystem include"
n=0; extra=0
for f in $(find lib/libc/src -name '*.c' | sort); do
    $GCC -fsyntax-only -ftrack-macro-expansion=0 $F -Wconversion "$f" > "$out/g.err" 2>&1 || continue
    "$EMBCC" --target=x86_64-elf -fsyntax-only $F -Wconversion "$f" > "$out/e.err" 2>&1 ||
        fail "$f does not compile"
    grep "^$f:" "$out/g.err" | pick > "$out/g"
    grep "^embcc: $f:" "$out/e.err" | pick > "$out/e"
    if [ -n "$(comm -13 "$out/g" "$out/e")" ]; then
        comm -13 "$out/g" "$out/e" | sed "s#^#  $f:#"; extra=1
    fi
    n=$((n + 1))
done
[ $extra = 0 ] || fail "warnings GCC does not give, on lib/libc"
[ $n -gt 50 ] || fail "only $n lib/libc files compiled under $GCC"
echo "warnings-conversion: no warning GCC does not give, over $n lib/libc sources"
