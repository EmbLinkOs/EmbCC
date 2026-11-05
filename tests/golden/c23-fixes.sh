#!/bin/sh
# C front-end gaps found after the 16-feature work, each held here:
#
#   1. a member of a constexpr structure is a named constant (C23 6.6p7),
#      at file scope and in a block, a member the list leaves out is 0,
#      and a later declaration of the name hides it; a constexpr object
#      is const;
#   2. typeof and auto of an array compound literal, (int[]){ 1, 2, 3 };
#   3. digraphs (C95): <: :> <% %> %: %:%:, in code and in directives;
#   4. translation phase 2 before comments: a backslash-newline continues
#      a // comment, and --warn=comment says so;
#   5. C23 identifiers are XID_Start then XID_Continue (UAX #31); C17's
#      stay Annex D's;
#   6. the standard's macros: __STDC_IEC_559__ (not on AVR),
#      __STDC_UTF_16__ and _32__, __STDC_HOSTED__ 0 under --freestanding;
#   7. a static initializer's constant ?: picks an address constant, and
#      "str"[k] folds;
#   8. -P drops -E's line markers; file-scope register says why it is
#      refused.
set -u
echo "TEST-MARKER c23-fixes"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/c23-fixes
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) H=arm64-apple-darwin ;;
    Linux-x86_64) H=x86_64-linux-gnu ;;
    *) H= ;;
esac
# run NAME SRC [FLAGS]: built for the host and run, its status 0
run() {
    n=$1 src=$2; shift 2
    printf '%s\n' "$src" > "$out/$n.c"
    "$EMBCC" --target=$H "$@" "$out/$n.c" -o "$out/$n" > "$out/$n.err" 2>&1 ||
        fail "$n does not build: $(head -3 "$out/$n.err")"
    "$out/$n" || fail "$n exits $?"
}
# refuse NAME SRC WANT [FLAGS]: a compile error saying WANT
refuse() {
    n=$1 src=$2 want=$3; shift 3
    printf '%s\n' "$src" > "$out/$n.c"
    if "$EMBCC" -fsyntax-only "$@" "$out/$n.c" > "$out/$n.err" 2>&1; then
        fail "$n was accepted"
    fi
    grep -q -- "$want" "$out/$n.err" || fail "$n: not '$want': $(head -2 "$out/$n.err")"
}

# ---- 1. constexpr members ---------------------------------------------
if [ -n "$H" ]; then
run cxm '
constexpr struct { int w, h; struct { int x; } in; } SIZE = { 320, .in = { 7 } };
static_assert(SIZE.w * 2 == 640);
static_assert(SIZE.h == 0);
static_assert(SIZE.in.x == 7);
int arr[SIZE.in.x];
int f(void) {
    constexpr struct { long a; } L = { 5 };
    static_assert(L.a == 5);
    int t[L.a];
    return (int)(sizeof t / sizeof t[0]);
}
int main(void) { return sizeof arr / sizeof arr[0] == 7 && f() == 5 ? 0 : 1; }' --std=c23
fi
refuse cxshadow 'void f(void) { constexpr struct { int a; } L = { 5 };
    { struct { int a; } L = { 9 }; static_assert(L.a == 9); } }' \
    'constant integer expression' --std=c23
refuse cxconst 'void f(void) { constexpr int k = 3; k = 4; }' \
    "read-only 'k'" --std=c23
echo "constexpr: members are named constants (a hidden one is not); the object is const"

# ---- 2. typeof and auto of an array compound literal ------------------
"$EMBCC" --std=c23 -fsyntax-only -x c - <<'EOF' 2> "$out/cl.err" ||
void f(void) {
    auto a = (int[]){ 1, 2, 3 };
    static_assert(sizeof(typeof((int[]){ 1, 2, [5] = 3 })) == 6 * sizeof(int));
    static_assert(sizeof(typeof((char[]){ "abc" })) == 4);
    (void)a;
}
EOF
    fail "an array compound literal's type: $(head -2 "$out/cl.err")"
echo "typeof and auto see an array compound literal's type, counted from its list"

# ---- 3. digraphs ------------------------------------------------------
if [ -n "$H" ]; then
run dg '%:define CAT(a, b) a %:%: b
%:define STR(x) %:x
int CAT(ta, ble)<:3:> = <% 1, 2, 3 %>;
int main(void) <% return table<:2:> == 3 && STR(hi)<:1:> == 105 ? 0 : 1; %>'
fi
echo "digraphs: <: :> <% %> in code, %: and %:%: in directives and macros"

# ---- 4. a // comment a backslash-newline continues --------------------
printf 'int x; // continued \\\nint y = this is not code;\nint z;\n' > "$out/cs.c"
"$EMBCC" -c "$out/cs.c" -o "$out/cs.o" 2> "$out/cs.err" ||
    fail "a continued // comment was compiled as code: $(head -2 "$out/cs.err")"
[ ! -s "$out/cs.err" ] || fail "a continued // comment warned by default"
"$EMBCC" --warn=all -c "$out/cs.c" -o "$out/cs.o" 2> "$out/cs.err"
grep -q 'multi-line comment.*\[--warn=comment\]' "$out/cs.err" ||
    fail "--warn=all said nothing of the continued comment: $(cat "$out/cs.err")"
printf '/* one /* two */\nint v;\n' > "$out/nest.c"
"$EMBCC" --warn=comment -c "$out/nest.c" -o "$out/nest.o" 2> "$out/nest.err"
grep -q 'within a comment' "$out/nest.err" || fail "/* inside a comment was not warned"
echo "a backslash-newline continues a // comment, and --warn=comment says so"

# ---- 5. identifiers ---------------------------------------------------
# U+00B7 MIDDLE DOT continues an XID identifier; U+2160 ROMAN NUMERAL ONE
# starts one; U+00A8 DIAERESIS is Annex D's and no XID character
printf 'int a\302\267b = 1;\nint \342\205\240 = 2;\n' > "$out/xid.c"
"$EMBCC" --std=c23 -fsyntax-only "$out/xid.c" 2> "$out/xid.err" ||
    fail "C23 refused XID identifiers: $(head -2 "$out/xid.err")"
printf 'int a\302\250 = 1;\n' > "$out/ad.c"
"$EMBCC" --std=c17 -fsyntax-only "$out/ad.c" 2> "$out/ad.err" ||
    fail "C17 refused an Annex D identifier: $(head -2 "$out/ad.err")"
if "$EMBCC" --std=c23 -fsyntax-only "$out/ad.c" 2> /dev/null; then
    fail "C23 took U+00A8, which is no XID_Continue character"
fi
echo "C23 identifiers are UAX #31's; C17's are Annex D's"

# ---- 6. the standard's macros -----------------------------------------
pd() { "$EMBCC" "$@" -E -dM -x c /dev/null; }
pd --target=thumbv7em-none-eabi | grep -q '^#define __STDC_IEC_559__ 1$' ||
    fail "no __STDC_IEC_559__ on Cortex-M"
! pd --target=avr | grep -q '__STDC_IEC_559__' || fail "__STDC_IEC_559__ on AVR, whose double is 32 bits"
pd --std=c23 | grep -q '^#define __STDC_IEC_60559_BFP__ 202311L$' ||
    fail "no __STDC_IEC_60559_BFP__ under C23"
pd | grep -q '^#define __STDC_UTF_16__ 1$' || fail "no __STDC_UTF_16__"
pd | grep -q '^#define __STDC_UTF_32__ 1$' || fail "no __STDC_UTF_32__"
pd | grep -q '^#define __STDC_HOSTED__ 1$' || fail "__STDC_HOSTED__ is not 1"
pd --freestanding | grep -q '^#define __STDC_HOSTED__ 0$' ||
    fail "--freestanding left __STDC_HOSTED__ at 1"
pd -ffreestanding | grep -q '^#define __STDC_HOSTED__ 0$' ||
    fail "-ffreestanding left __STDC_HOSTED__ at 1"
echo "__STDC_IEC_559__ (not on AVR), __STDC_UTF_16/32__, __STDC_HOSTED__ 0 freestanding"

# ---- 7. constant ?: and "str"[k] in a static initializer ---------------
if [ -n "$H" ]; then
run si '#include <string.h>
static const char *const name = 2 > 1 ? "many" : "one";
static const char *const pick = "misra"[0] == (char)109 ? "misra" : "cert";
static const int first = "cert"[1];
int main(void) {
    return !strcmp(name, "many") && !strcmp(pick, "misra") && first == 101 ? 0 : 1;
}'
fi
echo "a static initializer takes a constant ?: and \"str\"[k], as GCC does"

# ---- 8. -P; file-scope register ---------------------------------------
printf '#define N 3\nint a[N];\n' > "$out/p.c"
"$EMBCC" -E -P "$out/p.c" > "$out/p.i" || fail "-P was refused"
! grep -q '^# [0-9]' "$out/p.i" || fail "-P left line markers: $(head -3 "$out/p.i")"
grep -q 'int a\[3\];' "$out/p.i" || fail "-P lost the code"
"$EMBCC" -E "$out/p.c" | grep -q '^# [0-9]' || fail "-E without -P has no line markers"
refuse reg 'register int r;' "not allowed here: C allows it on a block-scope object"
echo "-P drops -E's line markers; file-scope register is refused for what C says"
