# Our C++ runtime

*The Itanium C++ ABI, written against libgcc's unwinder.*

Code: [`lib/libcxx/`](../../lib/libcxx/README.md). Built with EmbCC
itself: `make libcxx`. Tested by `tests/golden/libcxx.sh`, on both
targets.

## Why

EmbCC has compiled GCC's **libstdc++** for a while — 193/193 objects on
both targets. That is the *headers*. This is the other half: the
**runtime** those headers call into, which was still GCC's `libsupc++`.

The same argument as for the C library applies, and more sharply. A
compiler that targets several operating systems cannot borrow one
toolchain's C++ runtime, because that runtime is where the compiler's own
output is interpreted: the RTTI objects `src/cxx/emit.c` writes, the
landing pads it generates, the guard variables it emits around static
locals. Those are EmbCC's decisions, and the code that reads them belongs
next to the code that writes them (R1).

## The split with libgcc

Walking the stack is the **unwinder**: decode `.eh_frame`, restore
callee-saved registers frame by frame, transfer control. It stays
libgcc's. It is language-neutral — every language on the platform shares
one — and `include/unwind.h` is EmbCC's declaration of its interface.

What is *not* neutral is the **policy**: given a frame, does this
exception match one of its catch clauses, and what address does the
handler receive? That question is about C++ types. The unwinder answers it
by calling back into `__gxx_personality_v0`, which is here.

libgcc owns the machine; this owns the meaning.

## What it provides

`operator new`/`delete` (all twenty spellings), static-local guards,
`std::type_info` and the ABI's type_info hierarchy,
`__dynamic_cast`, the exception object and the whole `__cxa_*` layer,
`__gxx_personality_v0`, and `std::terminate`.

Runtime headers: `<new>`, `<typeinfo>`, `<exception>`,
`<initializer_list>`. `<initializer_list>` is not optional and cannot be
replaced — `{1, 2, 3}` in a call is a *core language* construct whose type
is `std::initializer_list`, so the compiler requires that exact class with
those exact two members in that order.

## The standard library, so far

`<type_traits>`, `<utility>`, `<limits>`, `<iterator>`, `<memory>`,
`<functional>`, `<array>`, `<stdexcept>`, and the `<c*>` wrappers
(`<cstddef>`, `<cstdint>`, `<cstring>`, `<cstdlib>`, `<cstdio>`,
`<cmath>`, `<cctype>`, `<cerrno>`, `<ctime>`, `<csetjmp>`, `<cassert>`,
`<cinttypes>`, `<climits>`, `<cfloat>`). Tested by
`tests/golden/libcxx-std.sh`, which is mostly `static_assert`s — for a
compile-time library that is the strongest check there is, and a trait
that answers the wrong question does not crash, it silently selects the
wrong overload three layers up.

`<type_traits>` uses EmbCC's intrinsics wherever one exists, and that is
not an optimisation: most of these traits *cannot* be written in the
language. No expression tells you whether a class is polymorphic or
whether a constructor is trivial. The ones written out in C++ are the ones
that genuinely are expressible — the type-list membership tests, and the
transformations that are just partial specialisation.

Two things in them are worth reading for the reasoning rather than the
code. `<memory>`'s `uninitialized_*` algorithms all share one shape: if
constructing element *k* throws, the *k−1* already built are destroyed
before the exception leaves — without that, a vector that throws while
growing leaks every element it had copied. And `<stdexcept>`'s message is
**copied and reference-counted**: copied because a `what()` returning the
caller's pointer dangles exactly when it is read, during unwinding; and
reference-counted because `catch (logic_error e)` by value copies the
exception, and a copy constructor that allocates can throw — throwing
while unwinding is `terminate()`.

Writing these headers found six compiler bugs, which was rather the
point of writing them:

- **partial-specialisation matching read past the end of its argument
  array** whenever a deduced pack had two or more elements, because the
  already-flattened arguments were re-flattened using the flattened count
  over the *un*flattened array. The counts agree for a pack of nought or
  one, so `is_invocable<F, int>` worked and `is_invocable<F, int, double>`
  did not. That one construct underpins every detection idiom in a
  standard library.
- **`long double` had no case in the constant evaluator at all**, so
  `DBL_MAX` — which the compiler spells as a long double literal cast to
  double — was not a constant expression. Now folded exactly in the
  target's format rather than at double precision, so
  `1.0L + LDBL_EPSILON > 1.0L` is true, as it is on the machine.
- **a conditional was not folded** in a constant expression, making
  `int buf[(N > 4) ? N : 4];` a VLA or an error.
- **an overloaded unary `operator&` was ignored**, so `&x` on a type that
  overloads it gave the built-in address. That is the whole reason
  `std::addressof` exists — and the fix had to keep
  `__builtin_addressof` on the built-in path, and had to skip the lookup
  for an *incomplete* class, where `&x` is legal and no overload can be
  visible (libstdc++'s `<chrono>` does exactly that).
- **an rvalue reference bound to a function lvalue**, so for `ref(f)` the
  deleted `const T&&` guard that `std::ref` uses to reject temporaries
  beat the `T&` overload and `std::ref` of a function was rejected as
  deleted. A named function is an lvalue, so that candidate is not
  viable.
- and `_Noreturn` and `__func__` were missing from C entirely (found by
  the C library, and fixed with it).

## Five things that are easy to get wrong

**The two unwind passes are not the same pass.** Pass one only asks "is
there a handler anywhere above?" and changes nothing; only when one is
found does pass two run, destroying each frame on the way. Conflating them
is the classic personality-routine bug. Keeping them apart is what lets an
uncaught exception call `terminate()` with the throwing frame still intact
and inspectable, instead of after the stack has already been destroyed —
which is why a debugger can still show you where it came from.

**A match is not enough; the address has to be right too.** Catching a
`Derived` as `Base&` must hand the handler the address of the *base
subobject*, which under multiple inheritance is not the address of the
object. Catching `T*` must hand it the pointer's *value*, not the address
of the slot the value sits in. This implementation got the second one
wrong first: pointers were matched by the exact-type test, which returned
early and skipped the adjustment. Every pointer catch then received a
plausible-looking pointer to a pointer. The test that caught it prints
what the handler actually read.

**A rethrown exception is owned by the unwinder, not by the handler that
let it go.** `__cxa_end_catch` running as the stack unwinds out of a
handler that did `throw;` must pop the exception from the caught list and
*not* destroy it — the unwinder is still carrying it to whichever handler
keeps it next. Destroying it there is a use-after-free that reads
plausible memory nearly every time. Fixing that exposed the opposite
error: the handler count has to be written back even when it reaches
zero, or the next `__cxa_begin_catch` takes the rethrow path, counts the
exception as held twice, and it is then **never destroyed at all**.
Measured here as 0 destructor calls where the reference runtime makes 1.
Neither mistake is visible to any test of what a handler *does*, so
`tests/golden/libcxx.sh` counts destructor calls.

**Static destructors are not the C++ runtime's list.** `__cxa_atexit` and
`__cxa_finalize` live in the *C* library, beside `exit()`, because C++
static destructors and C `atexit` handlers must interleave by
**registration** order: an object constructed before an `atexit()` call
has to be destroyed after that handler runs. Two lists cannot express that
ordering however they are drained. This runtime had two lists at first,
and the result was not a subtle mis-ordering — the C++ destructors were
registered on a list nothing ever walked, so they simply never ran.

**"Exactly one" is the whole of `dynamic_cast`.** The cast succeeds iff
the most-derived object contains *exactly one* publicly reachable target
subobject. A search that returns its first hit passes every
single-inheritance test and is silently wrong the first time a class
inherits the same base twice. `tests/golden/libcxx.sh` builds that shape
deliberately and requires null.

## Status

C++ programs with virtual dispatch, RTTI, `dynamic_cast`, static locals,
`new`/`delete` and exceptions run on this runtime and our C library with
**no `libsupc++` and no newlib**, on x86-64 and aarch64, unchanged between
them.

Not yet here: `std::exception_ptr` and `std::nested_exception`, thread-safe
guards (the runtime is single-threaded throughout, and the ABI routes
every initialisation through `__cxa_guard_*` precisely so that becomes a
change to one file), and `__cxa_vec_*` (the compiler lowers array
new/delete itself and does not call them).

The **standard library** above this — `<type_traits>`, `<utility>`,
`<memory>`, `<string>`, the containers, `<algorithm>`, the iostreams — is
the next and much larger piece of work.
