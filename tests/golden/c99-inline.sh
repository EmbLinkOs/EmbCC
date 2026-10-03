#!/bin/sh
# C11 6.7.4p7: a non-static function whose file-scope declarations all
# say `inline` and none says `extern` has an INLINE DEFINITION in that
# unit. It provides no external definition: the optimizer may inline it,
# and a call that remains -- every call at -O0 -- names the external
# definition, which exactly one unit provides. The same header can then
# be included everywhere and the program still links.
#
# GNU89 has the opposite rule (`extern inline` is the inline-only form),
# selected by -fgnu89-inline, -std=gnu89 or __attribute__((gnu_inline)).
set -u
echo "TEST-MARKER c99-inline"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=$EMBCC_ROOT/tests/golden/out/c99-inline-$ARCH
rm -rf "${out:?}"; mkdir -p "$out"
fail=0

# What the object says about NAME: D defined, U undefined, - absent.
sym() {
    r=$(readelf -sW "$1" | awk -v n="$2" '$8 == n { print ($7 == "UND") ? "U" : "D"; exit }')
    echo "${r:--}"
}
# expect FILE.c NAME WANT [flags...]: compile, then check NAME.
expect() {
    src=$1; name=$2; want=$3; shift 3
    o="$out/$(basename "$src" .c).o"
    "$EMBCC" --target="$TARGET" -c "$src" -o "$o" "$@" || {
        echo "FAIL: $(basename "$src") $* did not compile"; fail=1; return; }
    got=$(sym "$o" "$name")
    case "$want" in
    *"$got"*) ;;
    *) echo "FAIL: $(basename "$src") $*: '$name' is $got, expected $want"
       fail=1 ;;
    esac
}

cat > "$out/twice.h" <<'CEOF'
inline int twice(int x) { return 2 * x; }
CEOF
cat > "$out/use.c" <<'CEOF'
#include "twice.h"
int use(int v) { return twice(v); }
long use_addr(void) { return (long)&twice; }
CEOF
cat > "$out/def.c" <<'CEOF'
#include "twice.h"
extern inline int twice(int);       /* this unit provides it */
long def_addr(void) { return (long)&twice; }
CEOF
cat > "$out/proto.c" <<'CEOF'
int twice(int);                     /* a declaration without inline */
#include "twice.h"
int p(void) { return twice(3); }
CEOF
cat > "$out/gnu.c" <<'CEOF'
extern inline __attribute__((gnu_inline)) int g_only(int x) { return x + 1; }
inline __attribute__((gnu_inline)) int g_ext(int x) { return x + 2; }
int gu(void) { return g_only(1) + g_ext(1); }
CEOF
cat > "$out/spell.c" <<'CEOF'
__inline__ int dbl(int x) { return 2 * x; }
extern __inline __attribute__((__gnu_inline__)) int inc(int x) { return x + 1; }
int sp(void) { return dbl(1) + inc(1); }
CEOF
cat > "$out/statics.c" <<'CEOF'
static inline int sq(int x) { return x * x; }
int s(void) { return sq(4); }
CEOF

# C99, the default.
expect "$out/use.c"   twice U -O0      # a call and an address remain
expect "$out/use.c"   twice U -O2      # the address still names it
expect "$out/def.c"   twice D -O0
expect "$out/def.c"   twice D -O2
expect "$out/proto.c" twice D -O0
# gnu_inline on the function: `extern inline` only inlines.
expect "$out/gnu.c"   g_only U -O0
expect "$out/gnu.c"   g_ext  D -O0
# GNU89 for the whole unit: plain `inline` is the external definition.
expect "$out/use.c"   twice D -O0 -fgnu89-inline
expect "$out/use.c"   twice D -O0 -std=gnu89
expect "$out/def.c"   twice D -O0 -fgnu89-inline  # the definition says inline only
expect "$out/use.c"   twice U -O0 -fgnu89-inline -fno-gnu89-inline
expect "$out/use.c"   twice D -O0 -std=gnu89 -fno-gnu89-inline  # as clang
# The GNU spellings are the same keyword (glibc's headers use these).
expect "$out/spell.c" dbl U -O0
expect "$out/spell.c" inc U -O0
# static inline is not affected: a local copy, or none.
expect "$out/statics.c" sq "D-" -O0
[ $fail = 0 ] && echo "inline definitions: never emitted; extern inline," \
    "a plain prototype or GNU89 rules emit one"

# Linked: one external definition, the same address seen from both units.
cat > "$out/main.c" <<'CEOF'
int printf(const char *, ...);
int use(int);
long use_addr(void);
long def_addr(void);
int main(void)
{
    printf("%d %d\n", use(21), use_addr() == def_addr());
    return 0;
}
CEOF
for o in -O0 -O2; do
    for f in use def main; do
        "$EMBCC" --target="$TARGET" $o -c "$out/$f.c" -o "$out/$f$o.o" ||
            { echo "FAIL: $f.c $o did not compile"; fail=1; }
    done
    if t_link "$out/prog$o" "$out/use$o.o" "$out/def$o.o" "$out/main$o.o" \
         > "$out/link$o.log" 2>&1; then
        r=$(t_run "$out/prog$o" 2>&1)
        [ "$r" = "42 1" ] || { echo "FAIL: $o printed '$r', expected '42 1'"
                               fail=1; }
    else
        echo "FAIL: $o: the two units did not link:"; cat "$out/link$o.log"
        fail=1
    fi
done
[ $fail = 0 ] && echo "linked at -O0 and -O2: one definition, one address"
exit $fail
