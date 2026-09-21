#!/bin/sh
# Our C++ runtime (lib/libcxx) — the Itanium C++ ABI, without libsupc++.
#
# EmbCC has compiled GCC's libstdc++ for a while; this is the other half of
# owning the stack -- the RUNTIME those headers call into. operator new,
# static-local guards, the type_info hierarchy, dynamic_cast, and the
# personality routine that decides whether a frame catches an exception.
#
# libgcc still supplies the UNWINDER, and that is the intended split rather
# than a gap: decoding .eh_frame and restoring registers frame by frame is
# language-neutral machinery every language on the platform shares. What is
# not neutral is whether a given catch clause matches a given exception,
# and that is what __gxx_personality_v0 below answers.
#
# The acceptance is behavioural: programs that exercise each piece, linked
# against our runtime and our libc with NO libsupc++ and NO newlib.
set -eu
echo "TEST-MARKER libcxx"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/libcxx
rm -rf "$out"; mkdir -p "$out"

arch=${ARCH:-x86_64}
LIB=build/libcxx/$arch/libcxx.a
CLIB=build/libc/$arch/libc.a
[ -f "$LIB" ] || { echo "skipped: $LIB absent (make libcxx)"; exit 0; }
[ -f "$CLIB" ] || { echo "skipped: $CLIB absent (make libc)"; exit 0; }

if [ "$arch" = x86_64 ]; then
    GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
    LD=${EMBCC_X86_LD:-x86_64-elf-ld}
    NEWLIB=$X86_NEWLIB
    H=tests/harness/x86_64
    PARTS="start sys crt"
else
    GCC=${EMBCC_AARCH64_GCC:-aarch64-elf-gcc}
    LD=${EMBCC_AARCH64_LD:-aarch64-elf-ld}
    NEWLIB=${EMBCC_AARCH64_NEWLIB:-$HOME/cross/newlib-aarch64-c99/aarch64-elf}
    H=tests/harness/aarch64
    PARTS="start semihost crt"
fi
command -v "$LD" > /dev/null 2>&1 || { echo "skipped: no $LD"; exit 0; }
LIBGCC=$(dirname "$($GCC -print-libgcc-file-name)")

# The harness's entry and syscall shims are scaffolding, not the thing under
# test -- the same ones tests/lib.sh uses. Everything above them is ours.
for part in $PARTS; do
    case $part in
        start) src=$H/start.S ;;
        crt)   src=$H/../crt.c ;;
        *)     src=$H/$part.c ;;
    esac
    $GCC -ffreestanding $( [ "$arch" = x86_64 ] && echo -mno-red-zone ) \
        -isystem "$NEWLIB/include" -c "$src" -o "$out/$part.o"
done

build_run() {              # build_run <source> <extra-flags>
    objs=""
    for part in $PARTS; do objs="$objs $out/$part.o"; done
    "$EMBCC" -c -x c++ "$2" -Ilib/libcxx/include -Ilib/libc/include \
        $( [ "$arch" = aarch64 ] && echo --target=aarch64-elf ) \
        "$1" -o "$out/p.o" 2> "$out/cc.txt" || { cat "$out/cc.txt"; return 91; }
    if [ "$arch" = x86_64 ]; then
        "$LD" -n -z max-page-size=0x1000 -T "$H/link.ld" -o "$out/p.64" \
            $objs "$out/p.o" "$LIB" "$CLIB" -L"$LIBGCC" -lgcc \
            2>&1 | grep -v 'RWX permissions' >&2 || true
        [ -f "$out/p.64" ] || return 92
        x86_64-elf-objcopy -I elf64-x86-64 -O elf32-i386 "$out/p.64" \
            "$out/p.elf"
    else
        "$LD" -T "$H/link.ld" -o "$out/p.elf" \
            $objs "$out/p.o" "$LIB" "$CLIB" -L"$LIBGCC" -lgcc \
            2>&1 | grep -v 'RWX permissions' >&2 || true
        [ -f "$out/p.elf" ] || return 92
    fi
    "$H/run.sh" "$out/p.elf" > "$out/run.txt" 2>&1
    return $?
}

want() {
    grep -qx "$1" "$out/run.txt" || { echo "FAIL: expected \"$1\""; exit 1; }
}

# ---- objects, vtables, guards and dynamic_cast ---------------------------
cat > "$out/objects.cc" << 'EOF'
#include <stdio.h>
#include <typeinfo>

struct Base { virtual ~Base(); virtual int id() const { return 1; } };
Base::~Base() {}
struct Derived : Base { int id() const override { return 2; } };
struct Other : Base { int id() const override { return 3; } };

/* Two bases, so the type_info is a __vmi_class_type_info and the runtime
 * has to add an offset rather than assume zero. */
struct Left { virtual ~Left() {} int l = 1; };
struct Right { virtual ~Right() {} int r = 2; };
struct Both : Left, Right { int b = 3; };

/* Two A subobjects, so a cast TO A has no single answer. The cast has to
 * come in sideways -- from Unrelated -- or the compiler would resolve it
 * statically and the runtime would never be asked. */
struct A { virtual ~A() {} };
struct B1 : A {}; struct B2 : A {};
struct Two : B1, B2 {};
struct Unrelated { virtual ~Unrelated() {} };
struct Wide : Two, Unrelated {};

/* Left is PRIVATE here, so it is not reachable from outside even though
 * the subobject is right there in the object. Reached sideways from Right
 * for the same reason. */
struct Priv : private Left, public Right {};

static int ctors;
struct Once { Once() { ctors++; } int v = 9; };
static int guarded() { static Once o; return o.v; }

int main()
{
    Base *b = new Derived;
    printf("virtual %d\n", b->id());
    printf("down %d cross %d\n", dynamic_cast<Derived *>(b) != nullptr,
           dynamic_cast<Other *>(b) == nullptr);
    printf("typeid %s\n", typeid(*b).name());
    delete b;

    Both both;
    Right *r = &both;
    /* A cross-cast between siblings: Left is not below Right, so this can
     * only work by finding the most-derived object first. */
    Left *l = dynamic_cast<Left *>(r);
    printf("sibling %d %d\n", l != nullptr, l ? l->l : -1);
    printf("adjusted %d\n", (void *)r != (void *)l);
    Both *back = dynamic_cast<Both *>(r);
    printf("mostderived %d\n", back == &both);

    Wide w;
    Unrelated *u = &w;
    printf("ambiguous %d\n", dynamic_cast<A *>(u) == nullptr);
    printf("unambiguous %d\n", dynamic_cast<B2 *>(u) != nullptr);

    Priv pv;
    Right *pr = &pv;
    printf("private %d\n", dynamic_cast<Left *>(pr) == nullptr);

    printf("guard %d %d %d\n", guarded(), guarded(), ctors);
    return 42;
}
EOF
if build_run "$out/objects.cc" -O2; then rc=0; else rc=$?; fi
cat "$out/run.txt"
[ "$rc" = 42 ] || { echo "FAIL: the objects program exited $rc"; exit 1; }
want "virtual 2"
want "down 1 cross 1"
want "typeid 7Derived"
# The Left subobject of Both is not at Right's address, so a cast that
# forgot to adjust would print 0 here and still "work".
want "sibling 1 1"
want "adjusted 1"
want "mostderived 1"
# Two A subobjects: the cast has no single answer, so it must be null
# rather than either of them. A search that returns its first hit passes
# every single-inheritance test and fails exactly here.
want "ambiguous 1"
want "unambiguous 1"
want "private 1"
# The static local is constructed once however many times the function runs.
want "guard 9 9 1"
echo "vtables, typeid, dynamic_cast down/across/to-most-derived, an
ambiguous base refused and a private one too, and a static local
constructed exactly once"

# ---- exceptions ----------------------------------------------------------
cat > "$out/exceptions.cc" << 'EOF'
#include <stdio.h>
#include <new>
#include <exception>
#include <typeinfo>

static char log_[64]; static int lp;
struct Trace { const char *n; ~Trace() { log_[lp++] = n[0]; } };

struct Err { int code; };
struct Derived : Err { Derived() { code = 7; } };

static void deep(int n)
{
    Trace t{"t"};
    if (n == 0) throw Derived();
    deep(n - 1);
}

struct Big { char pad[64]; int tag; Big() : tag(5) {} };

static int counted_dtors;
struct Counted {
    Counted() {}
    Counted(const Counted &) {}
    ~Counted() { counted_dtors++; }
};

int main()
{
    /* Thrown as Derived, caught as its base: the handler must receive the
     * Err subobject's address, not the object's. */
    int a = -1;
    try { deep(3); } catch (Err &e) { a = e.code; }
    printf("base %d cleanups %.*s\n", a, lp, log_);

    int b = 0;
    try {
        try { throw 42; }
        catch (...) { b = 1; throw; }        /* rethrow from catch (...) */
    } catch (int v) { b += v; }
    printf("rethrow %d\n", b);

    /* A rethrown exception is destroyed ONCE, by the handler that finally
     * keeps it -- not by the one that let it go, and not never.
     *
     * Counting is the only way to see this. Both ways of getting the
     * handler bookkeeping wrong are invisible to every other check here:
     * destroying it early is a use-after-free that reads plausible memory,
     * and losing count the other way leaks it silently. This number was 0
     * when it was first measured, against 1 from the reference runtime. */
    try {
        try { throw Counted(); }
        catch (Counted &) { throw; }
    } catch (Counted &) { }
    /* Exactly one: the throw constructs directly into the exception
     * object, and the handler that finally keeps it destroys it. */
    printf("rethrow-dtors %d\n", counted_dtors);

    /* A catch that does not match must let the exception past. */
    int c = 0;
    try { try { throw 3.5; } catch (int) { c = 99; } }
    catch (double d) { c = (int)d; }
    printf("skip %d\n", c);

    /* By value, and by pointer-to-base. */
    int d = 0;
    try { throw Big(); } catch (Big v) { d = v.tag; }
    Derived dv;
    int e = 0;
    try { throw (Err *)&dv; } catch (Err *p) { e = p->code; }
    printf("byvalue %d bypointer %d\n", d, e);

    int f = 0;
    try { (void)::operator new((unsigned long)-1); }
    catch (std::bad_alloc &x) { f = x.what()[5] == 'b'; }
    printf("badalloc %d\n", f);

    printf("uncaught %d\n", std::uncaught_exceptions());
    return a + b + c + d + e + f == 7 + 43 + 3 + 5 + 7 + 1 ? 42 : 1;
}
EOF
if build_run "$out/exceptions.cc" -O2; then rc=0; else rc=$?; fi
cp "$out/p.o" "$out/exceptions.o"     # kept for the linkage check below
cat "$out/run.txt"
[ "$rc" = 42 ] || { echo "FAIL: the exceptions program exited $rc"; exit 1; }
# Four frames of `deep` each ran their destructor on the way out. A
# personality routine that treated a cleanup frame as a handler frame would
# stop at the first one and print "t".
want "base 7 cleanups tttt"
want "rethrow 43"
want "rethrow-dtors 1"
want "skip 3"
want "byvalue 5 bypointer 7"
want "badalloc 1"
want "uncaught 0"
echo "throw and catch by base reference, by value and by pointer, cleanups
run in every intervening frame, rethrow from catch (...), a non-matching
handler declined, and operator new reporting failure as bad_alloc"

# ---- global destructors, and their order against C atexit ---------------
# The gap this closes: __cxa_atexit used to keep its own list in the C++
# runtime, so static destructors were registered and never run at all. They
# share the C library's atexit list now, and this is why that matters --
# the two kinds of handler have to interleave by REGISTRATION order, which
# two separate lists cannot express however they are drained.
cat > "$out/dtors.cc" << 'EOF'
#include <stdio.h>
#include <stdlib.h>

static char order[16]; static int n;
static void note(char c) { order[n++] = c; }

/* Declared FIRST, so it is constructed first and therefore destroyed
 * LAST -- which is the only position from which it can see every other
 * handler's mark. */
struct Report { ~Report() { printf("order %.*s\n", n, order); } };
static Report report;

struct Global { char c; Global(char x) : c(x) {} ~Global() { note(c); } };
static Global a('a');
static Global b('b');

static void c_handler() { note('C'); }

struct Late { ~Late() { note('L'); } };
static int touch() { static Late l; return 1; }

int main()
{
    atexit(c_handler);        /* registered after a, b and report */
    touch();                  /* constructs Late here, last of all */
    return 42;
}
EOF
if build_run "$out/dtors.cc" -O2; then rc=0; else rc=$?; fi
cat "$out/run.txt"
[ "$rc" = 42 ] || { echo "FAIL: the destructor program exited $rc"; exit 1; }
# Construction order: report, a, b, then Late (in main). Destruction is the
# reverse, with the C atexit handler in its registered position: Late, the
# handler, b, a -- and `report` last of all, which is how it can print this.
want "order LCba"
echo "global destructors run, in reverse order of construction, interleaved
with C atexit by registration order"

# ---- and none of it came from libsupc++ ----------------------------------
# The point of the exercise. If the reference libsupc++ were quietly
# supplying any of this, the tests above would pass and prove nothing.
"${LD%ld}nm" --undefined-only "$out/exceptions.o" > "$out/u.txt"
for sym in __cxa_throw __cxa_begin_catch __gxx_personality_v0 _Znwm; do
    grep -q "$sym" "$out/u.txt" ||
        { echo "FAIL: the test object does not even reference $sym"; exit 1; }
done
"${LD%ld}nm" "$LIB" | grep -q " T __gxx_personality_v0" ||
    { echo "FAIL: our library does not define the personality routine"; exit 1; }
echo "the programs call __cxa_throw, __gxx_personality_v0 and operator new,
and lib/libcxx is what defines them -- no libsupc++ was linked"
