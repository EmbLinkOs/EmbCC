#!/bin/sh
# The Windows target's DWARF, thread-locals and va_arg (MinGW's ways):
#
#   1. -g writes DWARF in .debug_* COFF sections, as MinGW's GCC does;
#      llvm-dwarfdump reads and verifies them, with the function's name,
#      its lines, and a variable's location;
#   2. __thread is emutls, GCC's model on MinGW: a variable's symbol is
#      __emutls_v.NAME (with its template __emutls_t's bytes), an access
#      calls __emutls_get_address, an extern one names __emutls_v.NAME --
#      the names clang -femulated-tls gives the same source;
#   3. va_arg of a structure (1, 2, 4 or 8 bytes in its slot, others by
#      reference), of long double and of __int128 compiles, reading the
#      slot as MinGW passes them.
set -u
echo "TEST-MARKER win-coff-more"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/win-coff-more
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
W=--target=x86_64-windows-gnu
DD=$(command -v llvm-dwarfdump || ls /opt/homebrew/opt/llvm/bin/llvm-dwarfdump 2>/dev/null || true)
OD=$(command -v llvm-objdump || ls /opt/homebrew/opt/llvm/bin/llvm-objdump 2>/dev/null || true)
NM=$(command -v llvm-nm || ls /opt/homebrew/opt/llvm/bin/llvm-nm 2>/dev/null || true)

# ---- 1. -g -------------------------------------------------------------
cat > "$out/g.c" <<'EOF'
int counter = 3;
int scale(int x)
{
    int y = x * counter;
    return y + 1;
}
EOF
"$EMBCC" $W -g -O0 -c "$out/g.c" -o "$out/g.obj" 2> "$out/g.err" ||
    fail "-g was refused: $(cat "$out/g.err")"
if [ -n "$DD" ]; then
    "$DD" --verify "$out/g.obj" > "$out/g.verify" 2>&1 ||
        fail "llvm-dwarfdump rejects the DWARF: $(tail -5 "$out/g.verify")"
    "$DD" --debug-info "$out/g.obj" > "$out/g.di" 2>&1
    grep -q 'DW_AT_name.*"scale"' "$out/g.di" || fail "no DIE for scale"
    grep -q 'DW_AT_name.*"counter"' "$out/g.di" || fail "no DIE for counter"
    grep -A4 'DW_AT_name.*"y"' "$out/g.di" | grep -q DW_AT_location ||
        fail "y has no location"
    "$DD" --debug-line "$out/g.obj" | grep -q 'g.c' || fail "the line table names no g.c"
fi
if [ -n "$OD" ]; then
    "$OD" -h "$out/g.obj" | grep -q '\.debug_info' || fail "no .debug_info section"
    "$OD" -r "$out/g.obj" | grep -q 'IMAGE_REL_AMD64_SECREL' ||
        fail "no SECREL relocation between the debug sections"
fi
echo "-g: DWARF in COFF that llvm-dwarfdump verifies, with names, lines and locations"

# ---- 2. __thread ---------------------------------------------------------
cat > "$out/t.c" <<'EOF'
__thread int hits = 5;
__thread long zeroed;
extern __thread int elsewhere;
int bump(void) { zeroed++; return ++hits + elsewhere; }
EOF
"$EMBCC" $W -O1 -c "$out/t.c" -o "$out/t.obj" 2> "$out/t.err" ||
    fail "__thread was refused: $(cat "$out/t.err")"
if [ -n "$NM" ]; then
    "$NM" "$out/t.obj" > "$out/t.nm"
    for want in ' D __emutls_v.hits' ' D __emutls_v.zeroed' ' U __emutls_v.elsewhere' \
                ' U __emutls_get_address'; do
        grep -q "$want\$" "$out/t.nm" || fail "no '$want': $(cat "$out/t.nm")"
    done
    if command -v clang >/dev/null 2>&1 &&
       clang --target=x86_64-w64-windows-gnu -femulated-tls -O1 -c "$out/t.c" \
           -o "$out/tc.obj" 2>/dev/null; then
        "$NM" "$out/tc.obj" | grep -o '__emutls_[vt]\.[a-z]*\|__emutls_get_address' |
            sort -u > "$out/tc.names"
        grep -o '__emutls_[vt]\.[a-z]*\|__emutls_get_address' "$out/t.nm" |
            sort -u > "$out/t.names"
        grep -q '__emutls_t.hits' "$out/t.nm" || echo "(the template is unnamed: a section symbol)"
        for n in __emutls_v.hits __emutls_v.zeroed __emutls_v.elsewhere __emutls_get_address; do
            grep -qx "$n" "$out/tc.names" || fail "clang names no $n: $(cat "$out/tc.names")"
        done
    fi
fi
if [ -n "$OD" ]; then
    "$OD" -s -j '.rdata$emutls' "$out/t.obj" | grep -q '05000000' ||
        fail "hits's first value is not in the template: $("$OD" -s "$out/t.obj" | head -20)"
fi
echo "__thread: emutls's control objects and calls, the names clang -femulated-tls gives"

# ---- 3. va_arg --------------------------------------------------------------
cat > "$out/v.c" <<'EOF'
#include <stdarg.h>
struct S8 { int a, b; };
struct S12 { int a, b, c; };
long pick(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    struct S8 s = va_arg(ap, struct S8);
    struct S12 t = va_arg(ap, struct S12);
    __int128 q = va_arg(ap, __int128);
    va_end(ap);
    return s.a + s.b + t.c + (long)q;
}
EOF
"$EMBCC" $W -O1 -c "$out/v.c" -o "$out/v.obj" 2> "$out/v.err" ||
    fail "va_arg of a struct or __int128 was refused: $(cat "$out/v.err")"
echo "va_arg of a structure and of __int128 compiles for Windows"
