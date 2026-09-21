#!/bin/sh
# C++ access control: private and protected enforced, and -- the half
# that costs more to get wrong -- every valid use of them still
# accepted.
#
# The asymmetry is the point of this file. Access control is the one
# feature in this compiler that can only REJECT programs; it cannot make
# a single working one work. So a missed refusal is a gap, and a wrong
# refusal is a compiler nobody can build with. Both halves are checked
# here, and the accepted half is the longer of the two on purpose: every
# case in it is a pattern the standard library actually relies on, and
# each one refused a correct program at some point while this was being
# written.
set -u
echo "TEST-MARKER cxx-access"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=$EMBCC_ROOT/tests/golden/out/cxx-access-$ARCH
rm -rf "$out"; mkdir -p "$out"
n_ref=0 n_ok=0

refuse() { # name source expected-message-grep
    src="$out/$1.cc"
    printf '%s\n' "$2" > "$src"
    if err=$("$EMBCC" --target="$TARGET" -c "$src" -o "$out/$1.o" 2>&1); then
        echo "FAIL $1: compiled instead of being refused"; exit 1
    fi
    echo "$err" | grep -q "$3" || {
        echo "FAIL $1: wrong diagnostic:"; echo "$err"; exit 1; }
    echo "$err" | grep -q "$src:" || {
        echo "FAIL $1: diagnostic has no file:line position:"; echo "$err"
        exit 1; }
    n_ref=$((n_ref + 1))
    # and -fno-access-control must compile the very same file
    "$EMBCC" --target="$TARGET" -fno-access-control -c "$src" \
        -o "$out/$1.o" 2> "$out/$1.noacc" || {
        echo "FAIL $1: -fno-access-control did not accept it:"
        cat "$out/$1.noacc"; exit 1; }
}

accept() { # name source
    src="$out/$1.cc"
    printf '%s\n' "$2" > "$src"
    "$EMBCC" --target="$TARGET" -c "$src" -o "$out/$1.o" 2> "$out/$1.err" || {
        echo "FAIL $1: refused a correct program:"; cat "$out/$1.err"; exit 1; }
    n_ok=$((n_ok + 1))
}

# ---- what must be refused ------------------------------------------------
refuse private-field \
    'class C { int p; public: C() : p(0) {} };
int f(C &c) { return c.p; }' \
    "'p' is private in 'C'"
refuse protected-field \
    'class C { protected: int q; public: C() : q(0) {} };
int f(C &c) { return c.q; }' \
    "'q' is protected in 'C'"
refuse private-member-function \
    'class C { void h() {} public: C() {} };
void f(C &c) { c.h(); }' \
    "'h' is private in 'C'"
refuse private-static \
    'class C { static int s; public: C() {} };
int f() { return C::s; }' \
    "'s' is private in 'C'"
# The pre-C++11 non-copyable idiom, which is the single most common use
# of a private member anywhere.
refuse private-copy-ctor \
    'class NC { NC(const NC &); public: NC() {} };
void f(NC &a) { NC b(a); }' \
    "constructor of 'NC' selected here is private"
refuse private-base-implicit \
    'struct B { int x; };
struct D : private B { };
B *f(D *d) { return d; }' \
    "'B' is an inaccessible base of 'D'"
# The original report this feature came from: a cast to a private base
# was accepted where g++ says the base is inaccessible.
refuse private-base-static-cast \
    'struct B { virtual ~B() {} };
struct D : private B { };
B *f(D *d) { return static_cast<B *>(d); }' \
    "'B' is an inaccessible base of 'D'"
# A PUBLIC edge leading to a private one: the single-edge check that
# every hand-written version of this starts with says yes here.
refuse private-base-two-levels \
    'struct B { int x; };
struct D : private B { };
struct M : public D { };
B *f(M *m) { return m; }' \
    "'B' is an inaccessible base of 'M'"
# Friendship is not transitive, and not inherited.
refuse friend-not-transitive \
    'class C { int p; friend struct F; public: C() : p(0) {} };
struct F { };
struct G { int peek(C &c) { return c.p; } };' \
    "'p' is private in 'C'"
refuse friend-not-inherited \
    'class C { int p; friend struct F; public: C() : p(0) {} };
struct F { };
struct H : F { int peek(C &c) { return c.p; } };' \
    "'p' is private in 'C'"

# ---- what must still be accepted -----------------------------------------
accept own-members \
    'class C { int p; void h() {} public: C() : p(0) {} int f() { h(); return p; } };'
accept derived-reaches-protected \
    'class B { protected: int q; void m() {} public: B() : q(0) {} };
class D : public B { public: int f() { m(); return q; } };'
accept friend-class \
    'class C { int p; friend struct F; public: C() : p(0) {} };
struct F { int peek(C &c) { return c.p; } };'
accept friend-function \
    'class C { int p; friend int peek(C &); public: C() : p(0) {} };
int peek(C &c) { return c.p; }'
# A nested class is a MEMBER of the enclosing one, so it reaches its
# privates -- and it is derived from nothing, which is why a checker
# built only on derivation refuses this.
accept nested-class-is-a-member \
    'class C { int p; public: C() : p(0) {} class In { public: int f(C &c) { return c.p; } }; };'
# A class template naming its own privates, including another instance
# of itself.
accept template-own-privates \
    'template <class T> class C {
    T p;
public:
    C() : p(T()) {}
    template <class U> T take(const C<U> &o) { return o.p; }
};
int f() { C<int> a; C<long> b; return (int)a.take(b); }'
# `template <class U> friend class X;` befriends every instance -- the
# shape shared_ptr and weak_ptr use to reach each other.
accept friend-template \
    'template <class T> class W;
template <class T> class S { T *p; friend class W<T>;
public: S() : p(0) {} };
template <class T> class W { public: T *get(S<T> &s) { return s.p; } };
int f() { S<int> s; W<int> w; return w.get(s) != 0; }'
# A using-declaration REPUBLISHES a base member at a new access. Both
# libstdc++'s vector (get_allocator from a protected base) and its
# internal_file_clock (_S_to_sys) depend on this compiling.
accept using-republishes-member \
    'struct B { protected: int get() const { return 1; } static int s() { return 2; } };
struct D : protected B { public: using B::get; using B::s; };
int f(D &d) { return d.get() + D::s(); }'
accept using-republishes-field \
    'struct B { protected: int m; };
struct D : protected B { public: using B::m; };
int f(D &d) { return d.m; }'
# Reaching an inherited member through a derived object is governed by
# that MEMBER's access, not by the base edge -- so a protected base does
# not make its public members unreachable when they were republished.
accept protected-base-published-member \
    'struct B { int pub() const { return 3; } };
struct D : protected B { public: using B::pub; };
int f(D &d) { return d.pub(); }'
# A private base is still reachable from inside the derived class.
accept private-base-from-inside \
    'struct B { int x; };
struct D : private B { B *up() { return this; } int f() { return x; } };'
# The standard library is full of this: a class befriending the
# machinery that builds it.
accept friend-of-nested \
    'class C { int p; public: C() : p(0) {} friend struct Maker; };
struct Maker { struct Inner { int f(C &c) { return c.p; } }; };'

echo "$n_ref ill-formed programs refused, each with a located diagnostic
and each accepted again under -fno-access-control; $n_ok correct ones
still compile, including every shape the standard library leans on"
