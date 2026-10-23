#!/bin/sh
# Nesting the compiler recurses on: 10,000 levels of parentheses, blocks,
# structures, unary operators, a left-deep chain of additions and a
# parenthesized declarator compile at -O0 and -O2 (GCC's limits-exprparen,
# limits-structnest and limits-declparen), and millions stop with an
# error rather than a segfault.
#
# 10,000 used to stop with that error, and limits-declparen.c (whose
# declarator was not measured) overflowed the 8 MB main-thread stack. The
# compiler runs on a thread with 256 MB of stack now (plat_run_on_stack);
# the error comes at 3/4 of whatever it has. A declarator scanned to its
# `)` at every level, and nested structures looked their tags up by a
# walk: both were quadratic, 40,000 levels taking 86 s and 3 s, and are
# timed below. The front end's, so x86_64-elf only.
set -u
echo "TEST-MARKER deep-nesting"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
if [ "$TARGET" != x86_64-elf ]; then
    echo "deep-nesting: the front end's; checked under x86_64-elf only"
    exit 0
fi
out=tests/golden/out/deep-nesting
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
command -v python3 > /dev/null 2>&1 || { echo "deep-nesting: no python3, skipped"; exit 0; }

gen() { # KIND N -> $out/KIND-N.c
    python3 - "$1" "$2" > "$out/$1-$2.c" <<'PYEOF'
import sys
k, n = sys.argv[1], int(sys.argv[2])
if k == 'paren':  print('int f(int x) { return ' + '(' * n + 'x' + ')' * n + '; }')
if k == 'block':  print('void f(void) { ' + '{' * n + '}' * n + ' }')
if k == 'struct': print(''.join('struct s%d { ' % i for i in range(n)) + 'int x; ' + '} m;' * n)
if k == 'unary':  print('int f(int x) { return ' + '- ' * n + 'x; }')
if k == 'add':    print('int f(int x) { return ' + 'x + ' * n + 'x; }')
if k == 'decl':   print('int ' + '(* ' * n + 'v' + ')' * n + ' = 0;')
PYEOF
}
limit() { perl -e '$t = shift; alarm $t; exec @ARGV' "$@"; }
for k in paren block struct unary add decl; do
    gen $k 10000
    for o in -O0 -O2; do
        limit 60 "$EMBCC" $o -c "$out/$k-10000.c" -o "$out/$k.o" > "$out/$k-10000.log" 2>&1
        st=$?
        [ $st -ne 142 ] || fail "$k, 10000 deep, $o: no answer in 60 s"
        [ $st -eq 0 ] || { head -3 "$out/$k-10000.log"; fail "$k, 10000 deep, $o: not compiled (exit $st)"; }
    done
    # far past what 256 MB holds whatever a level costs (a million
    # parentheses fit, just: the error must not hang on a frame's size);
    # a structure costs the most, and a million of them is past it too
    deep=5000000
    [ $k = struct ] && deep=1000000
    gen $k $deep
    limit 120 "$EMBCC" -c "$out/$k-$deep.c" -o "$out/$k.o" > "$out/$k-$deep.log" 2>&1
    st=$?
    [ $st -eq 1 ] || { head -3 "$out/$k-$deep.log"; fail "$k, $deep deep: exit $st, not 1"; }
    grep -q 'error: this nests too deeply to compile' "$out/$k-$deep.log" ||
        { head -3 "$out/$k-$deep.log"; fail "$k, $deep deep: no nesting error"; }
    rm -f "$out/$k-$deep.c"
done
# linear: 100,000 levels of each quadratic one in a few seconds
for k in decl struct; do
    gen $k 100000
    limit 20 "$EMBCC" -c "$out/$k-100000.c" -o "$out/$k.o" > "$out/$k-100000.log" 2>&1
    st=$?
    [ $st -ne 142 ] || fail "$k, 100000 deep: no answer in 20 s (quadratic again?)"
done
echo "deep-nesting: 10000 levels compile, millions stop with an error, for 6 forms; 100000 declarators and structures in seconds"

# `L'X'` is one token: a macro named L does not reach into it
printf '#define L 264\n#define u8 9\nint a = L + u8;\nint b = L'"'"'X'"'"';\nconst char *s = u8"x";\n' > "$out/prefix.c"
"$EMBCC" -E "$out/prefix.c" > "$out/prefix.i" 2>&1 || fail "cpp: prefix.c"
grep -q "int a = 264 + 9;" "$out/prefix.i" || { cat "$out/prefix.i"; fail "cpp: L and u8 as macros"; }
grep -q "int b = L'X';" "$out/prefix.i" || { cat "$out/prefix.i"; fail "cpp: L'X' expanded"; }
grep -q 'const char \*s = u8"x";' "$out/prefix.i" || { cat "$out/prefix.i"; fail "cpp: u8\"x\" expanded"; }
echo "deep-nesting: an encoding prefix stays with its literal"
