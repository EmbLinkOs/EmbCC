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
`<functional>`, `<array>`, `<vector>`, `<string>`, `<algorithm>`,
`<tuple>`, `<optional>`, `<numeric>`, `<map>`, `<set>`,
`<unordered_map>`, `<unordered_set>`, `<list>`, `<deque>`,
`<forward_list>`, `<queue>`, `<stack>`, `<string_view>`, `<span>`,
`<bitset>`, `<chrono>`, `<ratio>`, `<random>`, `<iostream>` and the rest
of the stream headers, `<stdexcept>`, and the `<c*>` wrappers
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

Two design decisions are worth stating. **`vector` doubles** its capacity,
so *n* appends cost O(*n*) rather than O(*n*²) — and when it grows it
*moves* the elements only if moving cannot throw, otherwise it copies.
That is the whole of its exception safety: a move constructor that throws
halfway through relocation has already hollowed out the old buffer and
there is nothing to roll back to, while a copy leaves the original
intact. It is why a type that wants fast vector growth must mark its move
constructor `noexcept` — not a hint, but the difference between moving
and copying.

**`string` keeps short strings inside the object** (15 characters and the
NUL, in 32 bytes) with the pointer aimed at its own buffer, so a short
string costs no allocation. "Is it short?" is `__p == __buf`, a pointer
comparison rather than a stolen capacity bit — one word larger than
libc++'s trick and free of any assumption about byte order. The cost is
that the object is *self-referential*: copying or moving one cannot copy
the three words, it has to re-aim the pointer, and a raw `const C *`
argument may point into the buffer that a reallocation is about to free.
`a += a` is the case that finds it, and the test does exactly that at
the short/long boundary.

**`<chrono>` puts the unit in the type**, and the conversion rule is the
part worth knowing: a duration converts *implicitly* only when nothing
is lost — seconds to milliseconds yes, milliseconds to seconds no.
Narrowing needs `duration_cast`, which *truncates* toward zero. That
asymmetry is not fussiness; it is what stops a timeout expressed in
milliseconds from silently becoming zero seconds.

**`<random>` separates the engine from the distribution**, which is what
makes `rand() % n` avoidable. Every distribution here *rejects* rather
than folds: the engine's range is split into whole buckets and a draw in
the ragged remainder is thrown away and redrawn. `% n` instead makes the
first `range % n` values one draw more likely than the rest — invisible
for a die and a 64-bit engine, always present, and unnecessary.
`random_device` refuses when the OS has no entropy rather than falling
back to a timestamp: a caller handed a predictable seed cannot tell, and
one handed an exception can decide.

**`string_view` and `span` own nothing**, which is both the point and
the hazard: passing one costs two words instead of a copy, and outliving
the characters it names is a dangling read that looks exactly like a
working program until the buffer is reused. The rule that follows is
worth stating: a view *parameter* is safe; a view *member* or *return
value* is a promise about a lifetime you must be able to keep.
`string_view` is also not guaranteed NUL-terminated, which is why it has
`data()` and no `c_str()`.

**`bitset::operator[]` returns a proxy**, because a bit has no address
for a `bool &` to point at. It is the canonical example of the pattern —
`vector<bool>` is the other, and the reason that one is disliked. The
bits above *N* in the last word are kept zero, or `count()`, `all()` and
`==` all lie after a `flip()`; the test uses `bitset<70>` so the
trimming is exercised rather than assumed.

**`list` uses a sentinel node**, so every operation is pointer-shuffling
with no special case for "the list is empty" or "this is the first
element" — the cases that are otherwise half the code and all of the
bugs. Because the sentinel is a *member*, moving a list has to re-close
the ring around the new object's own sentinel rather than copying three
words; the test moves one and then keeps using both.

**`deque` here is a ring buffer, not the map-of-blocks a full
implementation uses**, and the difference is visible: the standard says
a reference to an element survives a push at either *end*, and this does
not — growing moves everything. That is stated rather than glossed. What
it does give is the reason to reach for a deque at all: O(1) at both
ends with random access, where a vector is O(*n*) at the front.

**`forward_list`'s interface looks wrong until the reason is clear.**
`insert_after`, `erase_after`, `before_begin`: a singly-linked node
cannot reach its predecessor, so an operation *at* a position would have
to walk from the head — turning O(1) insertion into O(*n*) and defeating
the point. Naming the operations "after" is the honest interface for
what the structure can do.

**`shared_ptr` keeps two counts, and the second one is why `weak_ptr`
can exist.** The strong count decides when the *object* dies; the weak
count decides when the *control block* does. They must be separate
because a `weak_ptr` has to be able to ask "is it still alive?" after
the object is gone, which means the block outlives what it describes.
`lock()` is the only safe way to use one — it takes a strong reference
and tells you whether it could, where testing `expired()` and then
dereferencing is a race even single-threaded.

**The unordered containers use separate chaining**, for the same reason
the ordered ones use a tree: they promise that a reference to an element
survives a rehash, and open addressing moves elements on insert. A
rehash *relinks* the nodes rather than reallocating them, and the test
holds a reference across 500 insertions to prove it. The bucket index is
a mask on a power-of-two count, so the hash is *mixed* first —
`std::hash` on an integer is the identity, and without mixing the keys
0, 8, 16… would all land in one bucket. The test inserts 2000
sequential integers for exactly that reason.

**A streambuf is not "an object with a virtual write."** It owns two
windows into a character sequence, and the fast path is a *pointer bump*
inside the current window with no virtual call at all; the virtuals exist
only for the moment a window runs out. That is why `cout << c` in a loop
is not one virtual call per character, and it is the whole reason the
indirection is shaped the way it is.

Above it, two rules are worth knowing because they catch everyone once.
The **sentry** is what skips leading whitespace before a formatted
extraction — which is why `in >> a >> b` reads across lines without
anyone writing a skip — and what honours `unitbuf` on the way out.
And **width is consumed** by the next insertion while fill and precision
are sticky, which is why `setw` has to be repeated in a loop and
`setprecision` does not.

The standard streams sit on the C library's `FILE *` rather than on the
OS directly, because the C library already owns the buffering and two
buffers over one descriptor interleave wrongly the moment a program
mixes `printf` and `cout`. They live in raw storage that is never
destroyed: a static object in another translation unit may write to
`cout` from its *destructor*, and "after cout's destructor" must not be
reachable. Leaking three objects at process exit is the accepted trade,
and is what every implementation makes.

**The ordered containers share one red-black tree** (`include/__tree`):
a `map` is that tree keyed on a pair's first member, a `set` is it with
the key and the value the same thing. Red-black rather than something
simpler because the containers promise not only O(log *n*) but that an
iterator stays valid until its own element is erased — and that second
promise is the one people rely on, since it is what lets a loop erase
while iterating. Erasing a node with two children therefore *relinks*
the successor rather than copying its value, which would silently
invalidate an iterator to it.

The test checks the tree's invariants **directly**, at *n* = 1000, after
ascending, descending and interleaved insertion and then erasing back to
empty. That is deliberate: a tree that is merely "sorted and works" can
be arbitrarily unbalanced and still pass every functional test, so the
property that makes the complexity guarantee true has to be asserted and
not inferred.

**`sort` is an introsort**, because the standard requires O(*n* log *n*)
in the *worst* case and plain quicksort does not give it. The recursion
depth is counted and heapsort takes over past 2 log *n* — quicksort's
speed with heapsort's guarantee — and below sixteen elements it switches
to insertion sort, where the constant factors win. The test sorts the
three patterns that turn a naive quicksort quadratic (already sorted,
reversed, all equal) at *n* = 2000.

One detail in that header is worth singling out because no test of a
*value* can see it: `min` and `max` both return their **first** argument
when the two are equivalent, and `max_element` returns the **first**
maximum. Algorithms built on them inherit that tie behaviour, and it is
what decides whether a sort is stable. Writing this, the code was right
and the comment beside it was wrong; g++ settled it.

Two more things are worth reading for the reasoning rather than the
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
