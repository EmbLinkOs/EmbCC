#!/bin/sh
# -fbounds-safety (src/ir/irgen.c's bounds_check): run natively at -O0 and
# -O2. b.c's mode 0 stays in bounds (including &g[10], one past the end,
# which C allows); modes 1-5 each overrun one way -- a global array, a row
# of a two-dimensional local, a counted_by flexible array member, a
# counted_by pointer member, a negative index -- and must trap. c.c is
# clang's <ptrcheck.h> spelling (__counted_by between `*` and the name).
# d.c: __builtin_dynamic_object_size of a counted_by member is the count.
set -u
echo "TEST-MARKER bounds-safety"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/bounds-safety
rm -rf "$out"; mkdir -p "$out"
src=$PWD/tests/golden/bounds
fail() { echo "FAIL: $*"; exit 1; }
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) T=arm64-apple-darwin ;;
    Linux-x86_64) T=x86_64-linux-gnu ;;
    *) echo "skipped: no native target"; exit 0 ;;
esac
rc_of() { sh -c '"$1" "$2" > /dev/null 2>&1; echo $?' sh "$@" 2> /dev/null; }
for O in -O0 -O2; do
    "$EMBCC" --target=$T $O -fbounds-safety "$src/b.c" -o "$out/b" || fail "build $O"
    r=$("$out/b" 0) ; [ "$r" = "ok 10" ] || fail "$O: in bounds gave '$r'"
    for k in 1 2 3 4 5; do
        rc=$(rc_of "$out/b" $k)
        [ "$rc" -ge 128 ] || fail "$O: mode $k did not trap (rc $rc)"
    done
    "$EMBCC" --target=$T $O -fsanitize=bounds "$src/b.c" -o "$out/b2" ||
        fail "-fsanitize=bounds $O"
    rc=$(rc_of "$out/b2" 1); [ "$rc" -ge 128 ] || fail "-fsanitize=bounds did not trap"
    "$EMBCC" --target=$T $O "$src/b.c" -o "$out/b0" || fail "plain build $O"
    r=$("$out/b0" 0); [ "$r" = "ok 10" ] || fail "$O plain: '$r'"
    "$EMBCC" --target=$T $O -fbounds-safety "$src/c.c" -o "$out/c" || fail "c.c $O"
    "$out/c"; rc=$?; [ "$rc" = 101 ] || fail "c.c in bounds gave $rc"
    rc=$(rc_of "$out/c" 1); [ "$rc" -ge 128 ] || fail "c.c: __counted_by pointer not checked"
done
"$EMBCC" --target=$T -O1 "$src/d.c" -o "$out/d" || fail "d.c"
[ "$("$out/d")" = "10 18446744073709551615" ] || fail "bdos: $("$out/d")"
echo "-fbounds-safety: in bounds runs, every overrun traps; bdos counts"
