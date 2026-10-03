#!/bin/sh
# With no -o, the output is named as GCC names it: the input's file name
# with its suffix replaced, in the CURRENT directory.
#
# -S wrote to standard output, so `cc -S x.c` left no x.s; -c wrote next
# to the input (src/a.o, where GCC writes ./a.o) and took a dot in a
# directory name for the suffix; and a .s input with no -o was written
# to a file called `o`.
set -u
echo "TEST-MARKER output-names"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/output-names
rm -rf "${out:?}"; mkdir -p "$out/src.v2" "$out/run"
case "$EMBCC" in /*) E=$EMBCC ;; *) E=$EMBCC_ROOT/$EMBCC ;; esac
printf 'int f(void) { return 1; }\n' > "$out/src.v2/a.c"
printf '\t.text\n\t.globl g\ng:\n\tbx lr\n' > "$out/src.v2/t.s"
printf 'int h(void) { return 2; }\n' > "$out/src.v2/noext"

cd "$out/run" || exit 1
"$E" -c ../src.v2/a.c || { echo "FAIL: -c did not compile"; exit 1; }
[ -f a.o ] && [ ! -f ../src.v2/a.o ] || {
    echo "FAIL: -c src.v2/a.c did not write ./a.o"; ls -R ..; exit 1; }
"$E" -S ../src.v2/a.c > stdout.txt || { echo "FAIL: -S did not compile"; exit 1; }
[ -f a.s ] && [ ! -s stdout.txt ] || {
    echo "FAIL: -S src.v2/a.c did not write ./a.s (and nothing to stdout)"; exit 1; }
"$E" -S ../src.v2/a.c -o - | grep -q 'f:' || {
    echo "FAIL: -S -o - did not write to standard output"; exit 1; }
"$E" --target=thumbv7m-none-eabi -c ../src.v2/t.s || {
    echo "FAIL: t.s did not assemble"; exit 1; }
[ -f t.o ] && [ ! -f o ] || { echo "FAIL: t.s did not give ./t.o"; ls; exit 1; }
"$E" -x c -c ../src.v2/noext || { echo "FAIL: noext did not compile"; exit 1; }
[ -f noext.o ] || { echo "FAIL: a name with no suffix did not give noext.o"; ls; exit 1; }
echo "output-names: -c and -S name their output as GCC does, in the current directory"
