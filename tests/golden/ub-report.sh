#!/bin/sh
# -fsanitize=undefined on a hosted target (lib/rt/ubsan.c): a failed
# check says what and where, as clang's and GCC's runtimes do, then stops;
# -fsanitize-recover=undefined reports each and goes on; -fsanitize-trap
# (with or without a list) keeps the trap, and so does a board.
set -u
echo "TEST-MARKER ub-report"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/ub-report
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) T=arm64-apple-darwin ;;
    Linux-x86_64) T=x86_64-linux-gnu ;;
    *) echo "skipped: no native target on $(uname -s)-$(uname -m)"; exit 0 ;;
esac

cat > "$out/u.c" <<'EOF'
#include <stdio.h>
int add(int a, int b) { return a + b; }
int shl(int a, int n) { return a << n; }
int neg(int a) { return -a; }
int main(int argc, char **argv)
{
    (void)argv;
    int big = 0x7fffffff;
    int r = add(big, argc);
    r += shl(1, 31 + argc);
    r += neg(-big - argc);
    printf("done %d\n", r != 12345);
    return 0;
}
EOF

# ---- stops at the first, saying where ----------------------------------
"$EMBCC" --target=$T --sanitize=undefined "$out/u.c" -o "$out/u" ||
    fail "the build"
"$out/u" > "$out/u.out" 2> "$out/u.err"; st=$?
[ $st -ne 0 ] || fail "the overflow did not stop the program"
grep -q 'u.c:2:[0-9]*: runtime error: signed integer overflow' "$out/u.err" ||
    fail "no report of the overflow: $(cat "$out/u.err")"
[ "$(grep -c 'runtime error' "$out/u.err")" = 1 ] ||
    fail "it went on past the first: $(cat "$out/u.err")"
! grep -q done "$out/u.out" || fail "the program ran on"
echo "a failed check says file:line:col and what, and stops"

# ---- recover: each one, then on ----------------------------------------
"$EMBCC" --target=$T --sanitize=undefined --sanitize-recover=undefined \
    "$out/u.c" -o "$out/r" || fail "the recover build"
"$out/r" > "$out/r.out" 2> "$out/r.err" || fail "the recovering program failed"
for want in 'u.c:2:.*signed integer overflow' 'u.c:3:.*shift count' 'u.c:4:.*negation of the most negative'; do
    grep -q "$want" "$out/r.err" || fail "--sanitize-recover lacks '$want': $(cat "$out/r.err")"
done
grep -q '^done ' "$out/r.out" || fail "the recovering program did not finish"
echo "-fsanitize-recover=undefined reports overflow, shift and negation, and goes on"

# ---- division by zero: reported before it happens ------------------------
# (a recovering program would then divide; x86 raises SIGFPE for that)
printf 'int quo(int a, int b) { return a / b; }\nint main(int c, char **v) { (void)v; return quo(7, c - 1); }\n' > "$out/d.c"
"$EMBCC" --target=$T --sanitize=undefined "$out/d.c" -o "$out/d" || fail "the division build"
"$out/d" 2> "$out/d.err" && fail "the division by zero did not stop the program"
grep -q 'd.c:1:[0-9]*: runtime error: division by zero' "$out/d.err" ||
    fail "no report of the division: $(cat "$out/d.err")"
echo "a division by zero is reported, and stops, before it divides"

# ---- the trap ------------------------------------------------------------
for f in -fsanitize-trap -fsanitize-trap=undefined -fsanitize-undefined-trap-on-error; do
    "$EMBCC" --target=$T --sanitize=undefined $f "$out/u.c" -o "$out/t" ||
        fail "$f: the build"
    "$out/t" > /dev/null 2> "$out/t.err"; st=$?
    [ $st -ne 0 ] || fail "$f: the trap did not stop the program"
    ! grep -q 'runtime error' "$out/t.err" || fail "$f reported instead of trapping"
done
# -fsanitize-trap=LIST turns no check on by itself
"$EMBCC" --target=$T -fsanitize-trap=undefined "$out/u.c" -o "$out/n" || fail "the trap-only build"
"$out/n" > /dev/null 2>&1 || fail "-fsanitize-trap=undefined alone checked something"
echo "-fsanitize-trap, with or without a list, traps; alone it checks nothing"

# ---- a board traps -------------------------------------------------------
"$EMBCC" --target=thumbv7em-none-eabi --sanitize=undefined -O1 -c \
    "$out/u.c" -o "$out/m.o" || fail "the board build"
OD=$(command -v llvm-objdump || ls /opt/homebrew/opt/llvm/bin/llvm-objdump 2>/dev/null || true)
if [ -n "$OD" ]; then
    ! "$OD" -t "$out/m.o" | grep -q '__embcc_ubsan_fail' ||
        fail "a board calls the hosted report"
    "$OD" -d "$out/m.o" | grep -q 'udf' || fail "a board's check does not trap"
fi
echo "a board's check traps, as before"
