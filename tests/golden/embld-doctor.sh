#!/bin/sh
# `embld --doctor` (docs/TOOLING.md T6): why the link failed.
#
# A linker prints "undefined reference to `foo'" and stops. That is the
# symptom; the cause is in the inputs, and the doctor reads them. Each case
# below is a different cause, and the test asserts the doctor names that
# cause -- not merely that it noticed something was undefined.
set -eu
echo "TEST-MARKER embld-doctor"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
[ -x "$EMBLD" ] || { echo "skipped: embld is not built (make embld)"; exit 0; }
out=tests/golden/out/embld-doctor
rm -rf "$out"; mkdir -p "$out"

# `caller` wants helper(); `helper` exists next door, but `static` keeps it
# in that file -- the case a plain "undefined reference" hides completely.
cat > "$out/caller.c" << 'EOF'
int helper(int);
int missing_entirely(void);
int caller(int x) { return helper(x) + missing_entirely(); }
EOF
cat > "$out/owner.c" << 'EOF'
static int helper(int x) { return x + 1; }
int use(void) { return helper(1); }
EOF
"$EMBCC" -c "$out/caller.c" -o "$out/caller.o"
"$EMBCC" -c "$out/owner.c"  -o "$out/owner.o"

"$EMBLD" --doctor "$out/caller.o" "$out/owner.o" > "$out/1.txt" 2>&1 && {
    echo "FAIL: doctor exited 0 with symbols undefined"; exit 1; }
cat "$out/1.txt"
grep -q "undefined: helper" "$out/1.txt" || { echo "FAIL: helper"; exit 1; }
grep -q "as .static." "$out/1.txt" || { echo "FAIL: no static diagnosis"; exit 1; }
grep -q "owner.o does define it" "$out/1.txt" || { echo "FAIL: not named"; exit 1; }
grep -q "nothing on this command line defines it" "$out/1.txt" ||
    { echo "FAIL: missing_entirely"; exit 1; }
echo "static-in-another-unit and nothing-defines-it: told apart"

# A C library name: the doctor knows the header that declares it, so it can
# say the library is missing rather than the definition.
cat > "$out/libc.c" << 'EOF'
unsigned long strlen(const char *);
int main(void) { return (int)strlen("hi"); }
EOF
"$EMBCC" -c "$out/libc.c" -o "$out/libc.o"
"$EMBLD" --doctor "$out/libc.o" > "$out/2.txt" 2>&1 || true
grep -q "C library's strlen (declared in <string.h>)" "$out/2.txt" ||
    { cat "$out/2.txt"; echo "FAIL: strlen not recognised"; exit 1; }
grep -q "libc.a" "$out/2.txt" || { echo "FAIL: no library named"; exit 1; }
echo "a C library name: '$(sed -n 's/^  it is the C library.s \(strlen\).*/\1/p' \
     "$out/2.txt" | head -1)' wants <string.h>'s library, not a definition"

# C++: a class whose key function is only declared emits no vtable, and a
# constructor is what asks for one. The symbol is mangled, so the doctor
# reads it back before explaining the rule that governs it.
cat > "$out/key.cc" << 'EOF'
struct Shape {
    Shape();
    virtual int area() const;          /* the key function: never defined */
    virtual ~Shape() {}
};
Shape::Shape() {}                      /* stores the vptr: wants the vtable */
int measure(Shape *p) { return p->Shape::area(); }
EOF
"$EMBCC" -c "$out/key.cc" -o "$out/key.o" 2>/dev/null ||
    { echo "skipped: C++ object would not build"; exit 0; }
"$EMBLD" --doctor "$out/key.o" > "$out/3.txt" 2>&1 || true
cat "$out/3.txt"
grep -q "that is vtable for Shape" "$out/3.txt" ||
    { echo "FAIL: _ZTV5Shape not read back"; exit 1; }
grep -q "KEY FUNCTION" "$out/3.txt" ||
    { echo "FAIL: the key-function rule is what explains a missing vtable"; exit 1; }
# The function itself: const survives the demangling, and the advice differs.
grep -q "that is Shape::area() const" "$out/3.txt" ||
    { echo "FAIL: _ZNK5Shape4areaEv not read back"; exit 1; }
grep -q "declared in the" "$out/3.txt" ||
    { echo "FAIL: a declared-and-never-defined member wants its own message"; exit 1; }
echo "C++: the mangled names read back, each with the rule that explains it"

# The C++ runtime's own symbols are not a key-function problem: they are a
# library that was not linked, and saying otherwise sends the reader to
# look for a virtual function that does not exist.
cat > "$out/rt.c" << 'EOF'
void __cxa_pure_virtual(void);
void _ZTVN10__cxxabiv117__class_type_infoE(void);
int main(void) { __cxa_pure_virtual();
                 _ZTVN10__cxxabiv117__class_type_infoE(); return 0; }
EOF
"$EMBCC" -c "$out/rt.c" -o "$out/rt.o"
"$EMBLD" --doctor "$out/rt.o" > "$out/4.txt" 2>&1 || true
[ "$(grep -c "belongs to the C++ runtime" "$out/4.txt")" = 2 ] ||
    { cat "$out/4.txt"; echo "FAIL: runtime symbols misdiagnosed"; exit 1; }
if grep -q "KEY FUNCTION" "$out/4.txt"; then
    echo "FAIL: key-function rule offered for a runtime symbol"; exit 1
fi
echo "libsupc++'s own symbols: 'link the runtime', not the key-function rule"

# And a link that would succeed has nothing to report.
cat > "$out/whole.c" << 'EOF'
int helper(int x) { return x + 1; }
int missing_entirely(void) { return 0; }
EOF
"$EMBCC" -c "$out/whole.c" -o "$out/whole.o"
"$EMBLD" --doctor "$out/caller.o" "$out/whole.o" > "$out/5.txt" 2>&1 || {
    cat "$out/5.txt"; echo "FAIL: clean link reported as broken"; exit 1; }
grep -q "every symbol referenced is defined" "$out/5.txt" ||
    { cat "$out/5.txt"; echo "FAIL: no clean verdict"; exit 1; }
echo "a link that would work: nothing to say"

# Every undefined symbol, not the first: the count at the end is the proof.
grep -q "doctor: 2 symbols undefined" "$out/1.txt" ||
    { echo "FAIL: not all reported at once"; exit 1; }
echo "all of them in one run, as the compiler's diagnostics do"
