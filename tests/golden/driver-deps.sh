#!/bin/sh
# The options a build system drives a compiler with (docs/tools/diagnostics.md T4):
# -M/-MM/-MD/-MMD/-MF/-MT/-MP write the make rule naming what the file
# included, which is how every C project tracks header dependencies, and
# -fsyntax-only checks without writing anything.
#
# The rule is checked three ways: it names the headers GCC names, `make`
# itself accepts it, and -MM leaves the system headers out where -M keeps
# them.
set -eu
echo "TEST-MARKER driver-deps"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/driver-deps-$ARCH
rm -rf "$out"; mkdir -p "$out/inc"
NL=$([ "$ARCH" = aarch64 ] && echo "$AARCH64_NEWLIB" || echo "$X86_NEWLIB")

cat > "$out/inc/b.h" << 'EOF'
#define B 2
EOF
cat > "$out/inc/a.h" << 'EOF'
#include "b.h"
#define A 1
EOF
cat > "$out/m.c" << 'EOF'
#include "a.h"
#include <stdio.h>
int main(void) { return A + B - 3; }
EOF

# 1. -MM: the project's own headers, not the system ones.
"$EMBCC" --target="$TARGET" -MM -MT prog.o -I "$out/inc" -isystem "$NL/include" \
    -c "$out/m.c" > "$out/mm.d"
grep -q "^prog.o:" "$out/mm.d" || { echo "-MT did not set the target:"; cat "$out/mm.d"; exit 1; }
for h in a.h b.h; do
    grep -q "$h" "$out/mm.d" || { echo "-MM lost $h:"; cat "$out/mm.d"; exit 1; }
done
grep -q "stdio.h" "$out/mm.d" && { echo "-MM kept a system header:"; cat "$out/mm.d"; exit 1; }
grep -q "_ansi.h" "$out/mm.d" && { echo "-MM kept a system header's own include"; exit 1; }
echo "-MM: the project's headers, no system ones"

# 2. -M keeps them, and names the same ones GCC names.
"$EMBCC" --target="$TARGET" -M -MT prog.o -I "$out/inc" -isystem "$NL/include" \
    -c "$out/m.c" > "$out/m.d"
grep -q "stdio.h" "$out/m.d" || { echo "-M lost the system header:"; cat "$out/m.d"; exit 1; }
if [ "$ARCH" = x86_64 ] && command -v x86_64-elf-gcc >/dev/null 2>&1; then
    x86_64-elf-gcc -M -MT prog.o -I "$out/inc" -isystem "$NL/include" \
        -c "$out/m.c" > "$out/m.gcc.d" 2>/dev/null
    norm() { tr ' \\' '\n\n' < "$1" | sed '/^$/d' | sed 's|.*/||' | sort -u; }
    norm "$out/m.d" > "$out/mine.list"
    norm "$out/m.gcc.d" > "$out/gcc.list"
    # Every header gcc names, EmbCC names. The reverse need not hold and
    # does not: EmbCC defines no __GNUC__, so newlib's __GNUC_PREREQ tests
    # take their portable branch and really do include <limits.h> where
    # gcc's build does not. The rule must describe the compile that
    # happened, which is exactly what this direction checks.
    missing=$(comm -23 "$out/gcc.list" "$out/mine.list")
    [ -z "$missing" ] || {
        echo "EmbCC's rule omits headers gcc names:"; echo "$missing"; exit 1; }
    echo "-M: every prerequisite gcc names ($(wc -l < "$out/gcc.list" | tr -d ' ') files), and"
    echo "    the ones its own #if branches add ($(wc -l < "$out/mine.list" | tr -d ' ') total)"
fi

# 3. -MMD writes the rule beside the object, and the object is still built;
#    -MP adds the bare rule per header that survives a deleted header.
rm -f "$out/m.o" "$out/m.d2"
"$EMBCC" --target="$TARGET" -MMD -MP -MF "$out/m.d2" -I "$out/inc" \
    -isystem "$NL/include" -c "$out/m.c" -o "$out/m.o"
[ -f "$out/m.o" ] || { echo "-MMD did not compile"; exit 1; }
grep -q "^$out/inc/a.h:$" "$out/m.d2" || {
    echo "-MP wrote no bare rule:"; cat "$out/m.d2"; exit 1; }
echo "-MMD -MF -MP: object built, rule written where asked"

# 4. make itself accepts the rule (the only judge that matters).
cat > "$out/Makefile" << EOF
all: prog.o
prog.o:
	@echo rebuilt
include m.d2
EOF
( cd "$out" && make -s all ) | grep -q rebuilt || {
    echo "make did not accept the generated rule"; exit 1; }
echo "make reads it"

# 5. -fsyntax-only: checked, nothing written — and a broken file still fails.
rm -f "$out/none.o"
"$EMBCC" --target="$TARGET" -fsyntax-only -I "$out/inc" -isystem "$NL/include" \
    -c "$out/m.c" -o "$out/none.o"
[ -f "$out/none.o" ] && { echo "-fsyntax-only wrote an object"; exit 1; }
printf 'int f(void) { return undeclared_thing; }\n' > "$out/bad.c"
if "$EMBCC" --target="$TARGET" -fsyntax-only -c "$out/bad.c" 2> "$out/bad.log"; then
    echo "-fsyntax-only passed a broken file"; exit 1
fi
grep -q "not declared" "$out/bad.log" || { echo "no diagnostic:"; cat "$out/bad.log"; exit 1; }
echo "-fsyntax-only: no object, and errors still reported"

# 6. -dumpmachine names the target that --target chose; --help lists what
#    the driver takes.
[ "$("$EMBCC" --target="$TARGET" -dumpmachine)" = "$TARGET" ] || {
    echo "-dumpmachine disagrees with --target"; exit 1; }
"$EMBCC" --help | grep -q -- "-fsyntax-only" || { echo "--help omits -fsyntax-only"; exit 1; }
"$EMBCC" --help | grep -q -- "-fdiagnostics-format" || { echo "--help omits the diagnostics options"; exit 1; }
echo "-dumpmachine and --help agree with the driver"
