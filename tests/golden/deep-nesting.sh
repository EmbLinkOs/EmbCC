#!/bin/sh
# Nesting the compiler recurses on: 2,000 levels of parentheses, blocks,
# structures, unary operators and a left-deep chain of additions compile;
# 10,000 (GCC's limits-exprparen.c and limits-structnest.c) stop with an
# error, where the host's stack overflowed with a segfault before. The
# front end's, so x86_64-elf only.
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
PYEOF
}
for k in paren block struct unary add; do
    gen $k 2000
    for o in -O0 -O2; do
        "$EMBCC" $o -c "$out/$k-2000.c" -o "$out/$k.o" > "$out/$k-2000.log" 2>&1 ||
            { cat "$out/$k-2000.log"; fail "$k, 2000 deep, $o: not compiled"; }
    done
    gen $k 10000
    "$EMBCC" -c "$out/$k-10000.c" -o "$out/$k.o" > "$out/$k-10000.log" 2>&1
    st=$?
    [ $st -eq 1 ] || { head -3 "$out/$k-10000.log"; fail "$k, 10000 deep: exit $st, not 1"; }
    grep -q 'error: this nests too deeply to compile' "$out/$k-10000.log" ||
        { head -3 "$out/$k-10000.log"; fail "$k, 10000 deep: no nesting error"; }
done
echo "deep-nesting: 2000 levels compile, 10000 stop with an error, for 5 forms"

# `L'X'` is one token: a macro named L does not reach into it
printf '#define L 264\n#define u8 9\nint a = L + u8;\nint b = L'"'"'X'"'"';\nconst char *s = u8"x";\n' > "$out/prefix.c"
"$EMBCC" -E "$out/prefix.c" > "$out/prefix.i" 2>&1 || fail "cpp: prefix.c"
grep -q "int a = 264 + 9;" "$out/prefix.i" || { cat "$out/prefix.i"; fail "cpp: L and u8 as macros"; }
grep -q "int b = L'X';" "$out/prefix.i" || { cat "$out/prefix.i"; fail "cpp: L'X' expanded"; }
grep -q 'const char \*s = u8"x";' "$out/prefix.i" || { cat "$out/prefix.i"; fail "cpp: u8\"x\" expanded"; }
echo "deep-nesting: an encoding prefix stays with its literal"
