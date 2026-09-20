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
`<bitset>`, `<chrono>`, `<ratio>`, `<random>`, `<variant>`, `<any>`,
`<compare>`, `<concepts>`, `<format>`, `<ranges>`, `<atomic>`,
`<regex>`, `<coroutine>`, `<source_location>`, `<bit>`, `<numbers>`,
`<version>`, `<system_error>`, `<mutex>`, `<thread>`,
`<condition_variable>`, `<shared_mutex>`, `<semaphore>`, `<latch>`,
`<barrier>`, `<future>`, `<iosfwd>`, `<execution>`, `<typeindex>`,
`<scoped_allocator>`, `<iostream>`
and the rest of the stream headers including `<fstream>`, `<stdexcept>`,
and the `<c*>` wrappers
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

**`<variant>`'s hard part is not the storage.** Changing the alternative
must destroy the old one before constructing the new, and if that
construction throws there is nothing valid left to hold — the
alternatives live in place and there is nowhere to put a fallback. That
is what `valueless_by_exception()` reports, and why the state exists.
`get` always checks: a variant's alternative can change, so an unchecked
`get` would be a type confusion rather than merely a null dereference,
which is why there is no member `operator*` as `optional` has.

**`<compare>` is an ABI, not a header.** `a <=> b` on built-in types
yields `std::strong_ordering` or `std::partial_ordering` *by name*, so
the compiler needs the class to exist before it can compile the
operator — which is why a program using `<=>` must include this header
even though it names nothing from it. EmbCC's lowering writes a signed
char at offset 0 (less −1, equivalent 0, greater 1, unordered −128) and
the header reads a signed char at offset 0. Neither file checks the
other, so `tests/golden/libcxx-std/compare.cc` constructs an ordering
with the compiler and reads it with the header: that test is the only
place the two meet.

Only a **literal 0** may be compared against an ordering, and that is
enforced rather than documented: the comparisons take a
`__detail::__cmp_unspec`, constructible only from a null pointer, so
`(a <=> b) == 1` is a compile error instead of a silent comparison
against something meaningless.

Writing the header found a real conformance gap. A defaulted
`operator<=>` **implicitly declares `operator==`**
([class.compare.default]/2) and EmbCC did not, so a class written with
the one line `auto operator<=>(const T &) const = default;` could be
ordered and could not be compared for equality — because `a != b` is
rewritten as `!(a == b)` and *never* as `(a <=> b) != 0`. The implicit
declaration is now made in `src/cxx/class.c` beside the other implicit
members, with the same access as the `<=>` and defaulted, so it is
defined only if it is used.

**`<concepts>` is built out of itself, and that is the point.**
`signed_integral` is spelled `integral<T> && is_signed_v<T>` rather than
`is_integral_v<T> && is_signed_v<T>`, which gives the same answers and
loses everything: a concept participates in *partial ordering* by
**subsumption**, computed over the conjunction it is written as. Two
overloads constrained by `integral` and by `signed_integral` are
ordered, so a `long` picks the second; two `enable_if`s are two
unrelated expressions and the same call is ambiguous. `common_with` and
`swappable` are the two that are deliberately narrower than the
standard's — the first because there is no `basic_common_reference`
customization point here, the second because the standard's is written
over a niebloid that finds a member or ADL `swap` first. Both say so in
the header rather than answering a question they cannot answer.

Writing them found the second real gap: `common_reference` did not
exist, and `common_reference_with` on `common_type` is *wrong* in a way
that only shows up on move-only types. `common_type_t<const T &, const
T &>` is `T` — it decays, which is a **copy** — so a non-copyable type
had no common type with *itself*, and `movable<T>` was false for every
move-only type in the language. `common_reference_t<const T &, const T
&>` is `const T &`, nothing is copied, and the question is answerable
for any type at all. `<type_traits>` now has it, built on the
standard's COND-RES (calling a reference-to-function returning `T`,
which is the only way to produce an expression of exactly `T`'s value
category — `declval<T>()` is always an xvalue).

**`<any>` is a hand-written vtable, and writing it out is the point.**
The type is erased from the *object* and kept in a table of
*functions*: one static table per stored type, holding destroy, copy,
move and `type_info`. A virtual base class would work and would force
every stored value onto the heap with a vptr; this leaves the stored
bytes alone, so a value that fits in four words and whose move cannot
throw lives inside the `any` and `any(42)` allocates nothing. The
nothrow-move condition is not a tuning knob — `any`'s own move is
`noexcept` and moves the stored value in place, so a move that threw
halfway would leave the `any` holding neither the old value nor the
new. `swap` goes through a temporary rather than exchanging the
buffers, because a value in the small buffer is at a *different
address* afterwards and only its move constructor knows how to put it
there; a `memcpy` is right for a trivially copyable type and silently
wrong for our own `string`, which points into itself. The test counts
constructions against destructions rather than checking round-trips: an
`any` that leaked every value it held would pass a round-trip test.

**`<format>` is `printf` with the types put back.** `printf("%d", x)` is
a promise the compiler cannot keep: the format string says `int`, `x` is
whatever it is, and when they disagree the result is undefined behaviour
that usually looks like a plausible number. Here the replacement field
says only *how* to print; *what* to print comes from the argument's own
type through a `formatter<T>` chosen by overload resolution, so there is
no `%d` to get wrong — and a user type gets a `formatter<T>` and then
works everywhere `format` does.

The implementation erases its arguments exactly once, into a small array
of {kind, value} pairs plus, for a user type, one function pointer that
knows how to format it. The formatting loop is `vformat`, a single
non-template function; the template part is only the erasure. That is
why this is not one instantiation per call site per argument pack, and
it is the same reason `printf` is small and iostreams are not.

The replacement-field grammar is complete — argument ids automatic and
manual, fill and align, sign, `#`, `0`, width, precision, nested `{}`
for width and precision, and the type characters for every fundamental
type. Two behaviours are easy to get wrong and are tested for that
reason: the **fill** is read by looking at the character *after* it, so
`{:<<5}` fills with `<` rather than meaning align-left; and `0` is an
*alignment*, not a fill, so `{:08.2f}` of −1.5 is `-0001.50` with the
sign outside the zeros, and an explicit alignment turns it off entirely.

What is **not** here is compile-time checking of the format string.
C++20's `format` takes a `format_string<Args...>` whose `consteval`
constructor parses the string against the argument types and rejects a
bad one at compile time. That needs the whole parser to be
constant-evaluable; ours checks at run time and throws `format_error`.
It is the one place this header is weaker than the standard's rather
than merely narrower.

Writing it found two more gaps. `<stdexcept>` had only the `const char *`
constructors, so a message that is *built* — which is nearly every
message worth throwing — could not be thrown without keeping the buffer
alive past the throw. And `<iterator>` had no **insert iterators** at
all, so `copy(a.begin(), a.end(), back_inserter(b))` did not compile;
without it the destination has to be sized first, and `copy` into an
empty vector is the single most common way to write out of bounds,
because nothing in `copy`'s signature says the destination must already
be big enough.

**`<ranges>` is about laziness, and the test measures it rather than
describing it.** `v | filter(odd) | transform(sq)` allocates nothing and
touches no element until it is iterated, because the composition is a
*type*, not a pipeline of containers; the same thing written with the
classic algorithms means two intermediate vectors and two full passes.
So the predicates and transforms in `rangeviews.cc` **count their
calls**, and the checks are on the counts: building the pipeline calls
the predicate zero times, `filter | transform` calls the transform five
times where `transform | filter` calls it fifteen, and `take(3)` over a
transform evaluates three elements rather than ten. A pipeline that
quietly materialised an intermediate would produce the right numbers and
fail every one of those.

The other half is honesty about **categories**. `filter_view` over a
random-access source is only *bidirectional*, because you cannot jump
*n* kept elements ahead without looking at the ones in between; an
iterator that claimed random access would make `std::advance` skip the
wrong elements silently, which is a bug this tree has already had once,
in `deque`. `transform_view` keeps its source's category exactly,
because applying *f* to element *n* costs what reaching element *n*
costs.

Two design notes. The adaptors are ordinary function objects rather than
niebloids, so ADL is not blocked — narrower than the standard, and
nothing here notices because every container has members. And `iota`'s
unboundedness is a **type**, not a flag: `end()`'s type depends on it,
so a runtime `bool` cannot express it — `end()` would have to return an
iterator either way, and the only iterator it could return is one equal
to `begin()`, making the range empty. That was this view's first bug and
it is why `unreachable_sentinel_t` exists.

Writing `<ranges>` found **two more compiler bugs**, both of which had
been latent for want of a program shaped like this:

- **A nested braced list was scored one element at a time.** Choosing an
  overload for `vector<vector<int>> v{{1, 2, 3}}` offered the inner
  `{1, 2, 3}` to `vector<int>`'s constructors *as three arguments*,
  never as one `initializer_list` — so it found no three-argument
  constructor and reported the whole outer call as having no viable
  candidate. The initialization itself was right all along, which is why
  `{{1, 2}}` worked (`vector(n, value)` happens to take two) and
  `{{1, 2, 3}}` did not: it rejected valid programs rather than
  miscompiling them. `ics_of` now tries the initializer-list
  constructors first, the order `construct` already used.

- **Two lambdas in two different blocks of one function mangled the
  same.** An unnamed type's number was counted per *block* scope, so the
  first lambda in every `{ }` came out `Ut_`. Invisible until such a
  type reaches a template argument — and then
  `v | filter(a)` in one block and `v | filter(b)` in another produce
  two distinct `filter_view` instantiations with identical mangled
  names, which the emitted C rejects as a redefinition. The counter now
  belongs to the enclosing function.

**`<atomic>` is built on the compiler's builtins, deliberately.** The
correct instruction for a sequentially consistent store is `xchg` on
x86-64 and `stlr` on aarch64, and that knowledge belongs to the back
end — a library emitting its own inline assembly would be duplicating
the one thing the compiler already knows and would be wrong on the next
target. So everything here is `__atomic_*`, and everything takes a
memory order with `seq_cst` as the default, so the cheap forms are
available and never accidental.

There is one thread here, so the test cannot look for a race. What it
can check is everything else, which is most of what goes wrong: that
`fetch_add` returns the value *before* and `++` the value *after* —
getting that backwards makes a ticket dispenser hand out the same number
twice, a bug that needs two threads to bite and one to write; that a
failed `compare_exchange` writes back what was **actually** there, which
is the whole reason its argument is a reference and the whole reason a
CAS loop terminates; and that every memory order reaches the compiler,
so a target missing one lowering fails here rather than in a kernel.

`wait`/`notify_one`/`notify_all` are **absent**, and so are `<thread>`
and `<mutex>`. They must *block*, which needs a futex or equivalent, and
`lib/libc/os/backend.h` has no such primitive. Writing them as spin
loops would be a correctness-preserving lie that burns a core.

Writing it found a **third compiler bug and a fourth gap**:

- **A conversion function inherited from a base was invisible to the
  built-in operators.** `a == 7` on a class whose `operator int()` comes
  from a base found no candidate at all, because the scan looked only at
  the class's own scope. `std::atomic<int>`, whose conversion lives in
  `__atomic_base`, could not be compared with anything.

- **A using-declared base member did not get the derived class's
  implicit object parameter** ([over.match.funcs]/4). `a = 3` was
  *ambiguous* against the implicit copy assignment: the first wanted a
  derived-to-base conversion for its object and an exact match for its
  argument, the second the reverse, and neither was better in every
  argument.

- **The generic atomic builtins refused anything but an integer or
  pointer.** They pass their values by *pointer* and copy bytes, so any
  object of a workable size will do — which is what
  `std::atomic<double>` and `std::atomic<SmallStruct>` need, and a
  tagged pointer under a 16-byte compare-and-swap is why anyone wants
  the second. `irgen` now lowers such an object as the unsigned integer
  of the same size; the instruction is identical either way.

**`<regex>` is a backtracker, and that is a decision rather than a
shortcut.** A Thompson NFA simulation runs in O(*nm*) and never blows
up; a backtracker can take exponential time, because `(a+)+b` against
*n* a's tries every way of splitting them. But an automaton cannot
support **backreferences** — `(a*)` requires remembering what a group
captured, and an automaton has no state for that. The standard's
grammar has backreferences, so an implementation of it is a
backtracker, and every real one is. What can be done is to *bound* it:
this one counts steps and raises `error_complexity` rather than
hanging.

The matcher is backtracking with an **explicit continuation list** — a
linked stack of "what remains", built on the C stack as the walk
descends. Making it explicit rather than a chain of closures is what
lets a group's *end* and a repetition's *next round* be frames of their
own, and those two are the awkward cases: a capture must be recorded
when the body finishes *and undone* if the match later fails past it,
and a repetition can only decide whether to go round again once its
body has matched.

Two behaviours are the ones an engine gets completely wrong, so the
test is weighted towards them. **Greedy against lazy**: `<.+>` on
"<a><b>" matches the whole thing and `<.+?>` matches "<a>", and getting
this backwards produces plausible output on simple inputs and destroys
any parser built on it. **Leftmost, not longest**: `(a|ab)` against
"ab" matches "a", because the alternation takes the first branch that
lets the *rest* succeed and here nothing follows — POSIX answers "ab"
to the same question, and the standard says ECMAScript. Every
expectation in `regexes.cc` was checked against libc++ before it was
checked against this implementation; the three places the grammars
genuinely disagree (`[]]`, a bare `{`, a backreference to a group that
never participated) are described in the test rather than asserted.

Two bugs it found in itself, both of the same shape — a decision made
too late. `regex_match` first tested "did it reach the end?" *after* a
match was found, so `(a|ab)c*` against "abc" failed: the engine settled
on "a" with no c's and was never asked to try "ab" with one. The
acceptance test belongs where backtracking can respond to it. And
`regex_replace` looped while `pos != e`, which stops one position
short — `x*` matches empty at *every* position including the last, so
"abc" is "-a-b-c-" with four replacements and not three.

**`<coroutine>` is the library half of a language feature.** EmbCC
compiles `co_await`/`co_yield`/`co_return` (`src/cxx/coro.c`) and cannot
do any of it without the names here: it looks up
`std::coroutine_traits` for the promise type and `std::coroutine_handle`
to hand one back, exactly as `<=>` needs `<compare>`. A
`coroutine_handle` is **one pointer** — not a smart pointer, not an
owner. `destroy()` frees the frame and every other handle to it dangles;
somebody has to own the coroutine, and that somebody is the return
object `get_return_object()` produced. The frame layout is an ABI
between this header and `src/cxx/emit.c` — two function pointers,
resume then destroy, with `done()` meaning "the resume pointer is null"
— and all four operations go through `__builtin_coro_*` so the layout is
stated in *one* place.

**`<source_location>` exists because `__FILE__` cannot be passed along.**
A logging *function* taking them reports its own position; only a macro
reports its caller's. `source_location::current()` as a **default
argument** is evaluated at the call site, which is a language rule
rather than a library trick, and is the whole reason the class exists.

**The threading headers are real, and say so honestly.** Every mutex
here is one atomic int and the seam's futex: the uncontended path never
enters the kernel, and only a thread that finds the lock held sleeps on
the word. That is why the seam offers a *futex* rather than a "mutex" —
the policy (recursion, fairness, try_lock) belongs in the library, where
it is written once, not in each target. The three states are the classic
ones, and two is not enough: unlock has to wake a sleeper and can only
know there is one if locking recorded it.

On a target with no threads, everything that does not need a second
thread still works — mutexes, `call_once`, `this_thread` — and
`std::thread`'s constructor throws `system_error`. That is the design
committing to an answer: running the function on the calling thread and
calling it a thread deadlocks the first time anything joins from inside
it.

One hole this opened and closed: a **bounded** wait must terminate. With
no futex there is no second thread, so a timed wait for something
another thread would have to do can never succeed — and polling a
deadline is worse than useless, because a target with no futex usually
has no clock either and `steady_clock::now()` then returns the same
value forever. The test hung until `__can_block()` was added. An
*unbounded* wait still waits: a program that blocks forever with no
other thread has deadlocked, and hanging is the honest report of its own
bug.

**`std::function` and `exception_ptr` were both missing**, and the second
is why nothing could cross a thread boundary: a worker has no caller to
throw to, so a failure has to be *captured* and rethrown where somebody
is waiting. That needed four `__cxa_` entry points and a `referenceCount`
in the exception header — the count is separate from `handlerCount` and
means a different thing, and the object dies when both are done.

**`<iosfwd>` caught a real mistake.** It declares the stream and string
templates with their default arguments, and every header that *defines*
one now includes it and repeats the parameters without defaults — which
is what the standard requires, and which this library was violating
until EmbCC learned to diagnose a repeated default template argument.

**`<fstream>` is exercised where a filesystem exists.** The QEMU harness
has an `open` that returns `ENOSYS`, and on such a target the *correct*
behaviour of every file operation is to fail — so the test probes first
and asserts that a stream which could not open reports `!is_open()` and
`failbit`, rather than pretending the target has files it does not. The
buffer logic above it is the same code the string streams exercise.

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
