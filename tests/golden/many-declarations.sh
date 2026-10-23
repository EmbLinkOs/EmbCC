#!/bin/sh
# Compile time grows with the number of declarations, not its square.
#
# sema found a function or a global by walking the unit's list of them
# with strcmp, once for every name an expression uses: GCC's
# limits-externdecl.c -- 100,000 extern functions, each referenced in one
# initializer -- ran for most of an hour, where GCC takes a second, and
# 10,000 took five seconds. Both lists are indexed by name now
# (find_func, find_global), so these compile in a second or two; one that
# has not finished in 120 s has gone quadratic again.
#
# The same held for typedefs, struct tags and a function's locals: each
# name was found by a walk of every typedef, every tag, and every local
# the function had declared, closed blocks' too. A unit of 5,000 vendor
# typedefs used 20,000 times took 3.75 s (0.70 s indexed), and the third
# unit below, 200,000 of each, did not finish in 120 s (6 s indexed).
set -u
echo "TEST-MARKER many-declarations"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/many-declarations
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# GCC's shape, and the same for objects: each declared, then all named
awk 'BEGIN {
    for (i = 0; i < 100000; i++) printf "extern int f%d(void);\n", i
    print "int (*const fns[])(void) = {"
    for (i = 0; i < 100000; i++) printf "    f%d,\n", i
    print "};" }' > "$out/functions.c"
awk 'BEGIN {
    for (i = 0; i < 100000; i++) printf "extern int g%d;\n", i
    print "int *const objs[] = {"
    for (i = 0; i < 100000; i++) printf "    &g%d,\n", i
    print "};" }' > "$out/objects.c"
# 200,000 typedefs and tags, each named in its own block of one function,
# whose local has the same name in every block
awk 'BEGIN {
    for (i = 0; i < 200000; i++) printf "typedef struct t%d { int v; } T%d;\n", i, i
    print "int f(void) {"
    print "    int s = 0;"
    for (i = 0; i < 200000; i++)
        printf "    { T%d x = { %d }; struct t%d y = x; s += y.v; }\n", i, i % 7, i
    print "    return s;"
    print "}" }' > "$out/typedefs.c"

limit() { perl -e 'alarm 120; exec @ARGV' "$@"; }
n=0
for t in x86_64-elf thumbv7m-none-eabi; do
    for s in functions objects typedefs; do
        limit "$EMBCC" --target=$t -c "$out/$s.c" -o "$out/$t-$s.o" ||
            fail "$t: $s.c (100,000 or more declarations) did not compile in 120 s"
        [ -s "$out/$t-$s.o" ] || fail "$t: $s.c gave no object"
        n=$((n + 1))
    done
done
echo "many-declarations: $n units of 100,000 or more declarations each compiled inside the time limit"
