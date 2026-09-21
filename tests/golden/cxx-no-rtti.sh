#!/bin/sh
# -fno-rtti, as g++ means it: no typeinfo object is written, a vtable's
# typeinfo slot is null, and typeid and dynamic_cast are refused. That is
# what lets a freestanding C++ program — the OS's, the kernel's — link
# against the C ABI alone, with no libsupc++ for __class_type_info's
# vtables. The same source with RTTI on must still carry its typeinfo.
set -u
echo "TEST-MARKER cxx-no-rtti"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/cxx-no-rtti-$ARCH
rm -rf "$out"; mkdir -p "$out"
NM=${NM:-${TARGET}-nm}
command -v "$NM" >/dev/null 2>&1 || NM=nm

cat > "$out/poly.cc" << 'EOF'
struct Shape {
    virtual ~Shape() {}
    virtual int sides() const = 0;
};
struct Tri : Shape { int sides() const override { return 3; } };
struct Quad : Shape { int sides() const override { return 4; } };
Shape *make(int n) { return n == 3 ? (Shape *)new Tri() : (Shape *)new Quad(); }
int sides_of(const Shape &s) { return s.sides(); }
EOF

"$EMBCC" --target="$TARGET" -fno-rtti -fno-exceptions -c "$out/poly.cc" \
    -o "$out/poly.nortti.o" || { echo "embcc -fno-rtti failed"; exit 1; }
"$EMBCC" --target="$TARGET" -c "$out/poly.cc" -o "$out/poly.rtti.o" || {
    echo "embcc failed"; exit 1; }

"$NM" "$out/poly.nortti.o" | grep -q "_ZTI\|_ZTS" && {
    echo "-fno-rtti still wrote a typeinfo:"
    "$NM" "$out/poly.nortti.o" | grep "_ZTI\|_ZTS"; exit 1; }
"$NM" "$out/poly.rtti.o" | grep -q "_ZTI3Tri" || {
    echo "with RTTI the typeinfo is missing"; exit 1; }
echo "no typeinfo under -fno-rtti; with it, _ZTI3Tri is there"

# The vtable's second word is the typeinfo pointer: null without RTTI. The
# .data.rel.ro/.data relocation for it is simply absent, so the word is 0.
if command -v "${TARGET}-objdump" >/dev/null 2>&1; then
    "${TARGET}-objdump" -r "$out/poly.nortti.o" | grep -q "_ZTI" && {
        echo "a relocation still names a typeinfo"; exit 1; }
    echo "no relocation names a typeinfo either"
fi

# g++ agrees on both halves of that (it is its own flag).
GXX=$([ "$ARCH" = aarch64 ] && echo "$AARCH64_REF_GXX" || echo "$X86_REF_GXX")/bin/$TARGET-g++
if [ -x "$GXX" ]; then
    "$GXX" -std=c++20 -fno-rtti -c "$out/poly.cc" -o "$out/poly.gxx.o" 2>/dev/null
    "$NM" "$out/poly.gxx.o" | grep -q "_ZTI\|_ZTS" && {
        echo "g++ -fno-rtti wrote a typeinfo, so this test's premise is wrong"
        exit 1; }
    echo "g++ -fno-rtti writes none either"
fi

# And the constructs that need RTTI are refused, with a position.
check() { # name source expected-message-grep
    src="$out/$1.cc"
    printf '%s\n' "$2" > "$src"
    if err=$("$EMBCC" --target="$TARGET" -fno-rtti -c "$src" -o "$out/$1.o" 2>&1); then
        echo "case $1: compiled instead of failing"; exit 1
    fi
    echo "$err" | grep -q "$3" || {
        echo "case $1: wrong diagnostic:"; echo "$err"; exit 1; }
    echo "$err" | grep -q "$src:" || {
        echo "case $1: diagnostic has no file:line position:"; echo "$err"; exit 1; }
    echo "case $1: refused with a diagnostic"
}
check typeid-nortti \
    'namespace std { class type_info {
public:
    virtual ~type_info();
    const char *name() const;
}; }
struct A { virtual ~A(); };
const char *f(A &a) { return typeid(a).name(); }' \
    "typeid with -fno-rtti"
check dyncast-nortti \
    'struct A { virtual ~A(); };
struct B : A {};
B *f(A *a) { return dynamic_cast<B *>(a); }' \
    "dynamic_cast with -fno-rtti"
