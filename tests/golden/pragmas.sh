#!/bin/sh
# The pragmas that change a program: once, push_macro/pop_macro, weak.
#
# All three were dropped without a word. A header guarded only by
# `#pragma once` was compiled twice ("redefinition of 'S'"); a header
# that saved a macro with push_macro, redefined it for its own text and
# restored it with pop_macro left its own definition in force for the
# program; and `#pragma weak` left the symbol strong, so the definition
# meant to replace it was a duplicate at link time.
set -u
echo "TEST-MARKER pragmas"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/pragmas
rm -rf "${out:?}"; mkdir -p "$out/inc"

# ---- #pragma once, reached by three spellings of the path ---------------
printf '#pragma once\nstruct S { int a; };\n' > "$out/inc/once.h"
printf '#include "inc/once.h"\n#include "./inc/once.h"\n#include <once.h>\nint f(void) { struct S s = { 3 }; return s.a; }\n' > "$out/once.c"
"$EMBCC" -I"$out/inc" -c "$out/once.c" -o "$out/once.o" 2> "$out/once.err" || {
    echo "FAIL: a #pragma once header was read again:"; cat "$out/once.err"; exit 1; }
echo "pragmas: #pragma once holds whichever path reaches the header"

# ---- push_macro and pop_macro, as directives and as _Pragma --------------
cat > "$out/pm.c" <<'SRC'
#define X 1
#pragma push_macro("X")
#undef X
#define X 2
int a = X;
#pragma push_macro("X")
#define X 3
int b = X;
#pragma pop_macro("X")
int c = X;
_Pragma("pop_macro(\"X\")")
int d = X;
#pragma push_macro("Y")
#define Y 5
#pragma pop_macro("Y")
#ifdef Y
int y_survived;
#endif
#pragma pop_macro("X")
int e = X;
SRC
"$EMBCC" -E "$out/pm.c" 2>&1 | grep -E '^int' | tr '\n' ' ' > "$out/pm.got"
want='int a = 2; int b = 3; int c = 2; int d = 1; int e = 1; '
[ "$(cat "$out/pm.got")" = "$want" ] || {
    echo "FAIL: push_macro/pop_macro: got '$(cat "$out/pm.got")'"
    echo "      want '$want'"; exit 1; }
echo "pragmas: pop_macro restores what push_macro saved, and an unmatched pop does nothing"

# ---- #pragma weak -------------------------------------------------------
command -v llvm-readelf > /dev/null 2>&1 || { echo "SKIP the rest: no llvm-readelf"; exit 0; }
cat > "$out/w.c" <<'SRC'
#pragma weak hook
void hook(void);
int impl(int x) { return x + 1; }
#pragma weak api = impl
int api(int);
int dflt(void) { return 1; }
#pragma weak dflt
int counter;
#pragma weak counter
int run(void) { if (hook) hook(); return api(1) + dflt(); }
SRC
"$EMBCC" --target=x86_64-elf -c "$out/w.c" -o "$out/w.o" || { echo "FAIL: w.c"; exit 1; }
llvm-readelf -s "$out/w.o" > "$out/w.sym"
for want in 'WEAK.*UND hook$' 'FUNC *WEAK.* api$' 'FUNC *WEAK.* dflt$' \
            'OBJECT *WEAK.* counter$' 'FUNC *GLOBAL.* impl$'; do
    grep -q "$want" "$out/w.sym" || {
        echo "FAIL: no symbol matching '$want':"; cat "$out/w.sym"; exit 1; }
done
# api is impl's address
a=$(grep ' api$' "$out/w.sym" | awk '{print $2}')
i=$(grep ' impl$' "$out/w.sym" | awk '{print $2}')
[ "$a" = "$i" ] || { echo "FAIL: api ($a) is not impl ($i)"; exit 1; }
# A strong definition elsewhere replaces the weak one at link time.
printf 'int dflt(void) { return 40; }\nint run(void);\nvoid _start(void) { for (;;) (void)run(); }\n' > "$out/s.c"
"$EMBCC" --target=x86_64-elf -c "$out/s.c" -o "$out/s.o" || { echo "FAIL: s.c"; exit 1; }
./embld -e _start -o "$out/w.elf" "$out/w.o" "$out/s.o" 2> "$out/ld.err" || {
    echo "FAIL: the strong dflt did not replace the weak one:"; cat "$out/ld.err"; exit 1; }
echo "pragmas: #pragma weak makes references, definitions, an alias and an object weak"

# C++ has no marker for it, so it is refused there by name.
printf '#pragma weak f\nvoid f();\n' > "$out/w.cc"
if "$EMBCC" -c "$out/w.cc" -o "$out/wcc.o" 2> "$out/wcc.err"; then
    echo "FAIL: #pragma weak was accepted in C++ and ignored"; exit 1
fi
grep -q 'not supported in C++' "$out/wcc.err" || {
    echo "FAIL: the C++ refusal does not say why:"; cat "$out/wcc.err"; exit 1; }
echo "pragmas: #pragma weak is refused in C++"
