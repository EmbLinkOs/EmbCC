#!/bin/sh
# <inttypes.h>'s format macros (lib/libc/include/inttypes.h) carry the
# length modifier of the type each one is for, on every target. A file
# that prints and scans every <stdint.h> type through its macros is
# preprocessed by EmbCC for each target -- the typedefs are then that
# target's own, spelled out -- and handed to clang, whose -Wformat matches
# a conversion to the exact type: int and long differ even where they are
# the same size, as long and long long do. clang reads it for a target of
# the same int, long and pointer sizes; the types in the file are explicit,
# so nothing else of the target matters.
set -u
echo "TEST-MARKER inttypes-formats"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
CLANG=${EMBCC_CLANG:-clang}
command -v "$CLANG" >/dev/null 2>&1 || { echo "SKIP: $CLANG absent"; exit 0; }
out=tests/golden/out/inttypes-formats
rm -rf "${out:?}"; mkdir -p "$out"

{
    echo '#include <inttypes.h>'
    echo 'int tprintf(const char *, ...) __attribute__((format(printf, 1, 2)));'
    echo 'int tscanf(const char *, ...) __attribute__((format(scanf, 1, 2)));'
    echo 'void f(void)'
    echo '{'
    for n in 8 16 32 64 LEAST8 LEAST16 LEAST32 LEAST64 \
             FAST8 FAST16 FAST32 FAST64 MAX PTR; do
        case $n in
        MAX) s=intmax_t; u=uintmax_t ;;
        PTR) s=intptr_t; u=uintptr_t ;;
        LEAST*) b=${n#LEAST}; s=int_least${b}_t; u=uint_least${b}_t ;;
        FAST*) b=${n#FAST}; s=int_fast${b}_t; u=uint_fast${b}_t ;;
        *) s=int${n}_t; u=uint${n}_t ;;
        esac
        echo "    { $s s = 0; $u u = 0;"
        echo "      tprintf(\"%\" PRId$n \"%\" PRIi$n, s, s);"
        echo "      tprintf(\"%\" PRIo$n \"%\" PRIu$n \"%\" PRIx$n \"%\" PRIX$n, u, u, u, u);"
        echo "      tscanf(\"%\" SCNd$n \"%\" SCNi$n, &s, &s);"
        echo "      tscanf(\"%\" SCNo$n \"%\" SCNu$n \"%\" SCNx$n, &u, &u, &u); }"
    done
    echo '}'
} > "$out/formats.c"

targets=$("$EMBCC" --reference=targets | sed -n 's/^| `\([^`]*\)`.*/\1/p')
n=0; fail=0
for t in $targets; do
    m=$("$EMBCC" --target="$t" -dM -E - < /dev/null 2>/dev/null)
    sz() { echo "$m" | sed -n "s/^#define $1 //p"; }
    case $(sz __SIZEOF_INT__)/$(sz __SIZEOF_LONG__)/$(sz __SIZEOF_POINTER__) in
    4/8/8) ct=x86_64-unknown-linux-gnu ;;
    4/4/4) ct=armv7-none-eabi ;;
    2/4/2) ct=avr ;;
    *) echo "$t: no clang target with its sizes"; fail=1; continue ;;
    esac
    "$EMBCC" --target="$t" -E "$out/formats.c" -o "$out/$t.i" 2> "$out/$t.err" || {
        echo "$t: does not preprocess:"; head -3 "$out/$t.err"; fail=1
        continue; }
    if "$CLANG" --target=$ct -fsyntax-only -Wformat -Werror=format -x c \
            "$out/$t.i" > "$out/$t.clang" 2>&1; then
        n=$((n + 1))
    else
        echo "$t: clang reads a macro as the wrong type:"
        grep 'error:' "$out/$t.clang" | head -4
        fail=1
    fi
done
[ $fail = 0 ] || exit 1
echo "inttypes-formats: every PRI and SCN macro matches its type on $n targets"
