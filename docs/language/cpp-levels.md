# C++ in EmbCC

The plan and the state of it. Decision and rationale: DECISIONS D-013.

**Goal:** C++20 with libstdc++, on x86-64 and aarch64, Itanium ABI,
link-compatible with g++ — and eventually compiled on EmbLinkOS itself.

**Shape:** `src/cxx/` is a C++ front-end that lowers to C; the C it produces
goes through EmbCC's existing C pipeline to either backend.

    foo.cc -> preprocessor (C++ predefined macros) -> src/cxx (parse + sema,
           -> lowered C) -> C sema -> IR -> opt -> src/arch/<arch> -> ELF

**Reference:** tools/build-ref-gxx.sh builds g++ 16.2 and a hosted libstdc++
against the harness's newlib, per target (~/cross/gcc-cxx-<target>). Every
C++ test is compiled by EmbCC and by that g++, run under QEMU on both
targets, and must agree; cross-ABI tests link an EmbCC half with a g++ half.

## Milestones

| | what | proven by |
|---|---|---|
| **CX1** | C++ as a better C: `bool`, `nullptr`, references, namespaces, `extern "C"`, overloading and Itanium mangling, default arguments, classes (members, methods, access, `this`), constructors and destructors (run on every scope exit), `new`/`delete`, static members, global constructors/destructors, function-local statics (guards) | tests/cxx, both targets, agreeing with g++; linking with libsupc++ |
| **CX2** | operator overloading, conversions (converting constructors, conversion operators, `explicit`), copy and move (implicit special members, rvalue references), temporaries and their lifetimes, classes by value in the ABI | cross-ABI tests with g++ |
| **CX3** | inheritance (single, multiple, virtual), virtual functions and vtables, pure virtuals, RTTI (`typeid`, `dynamic_cast`) | g++ calling EmbCC virtuals and back; each building the other's classes as virtual-base hierarchies |
| **CX4** | templates: class, function, alias and variable templates; deduction; explicit and partial specialization; SFINAE; variadics and fold expressions | every template symbol named as g++ names it |
| **CX5** | exceptions: `throw`/`try`/`catch`, unwind tables, the Itanium personality routine, `noexcept` | exceptions crossing EmbCC/g++ frames |
| **CX6** | the modern core: `auto`, `decltype`, lambdas, `constexpr` evaluation, range-`for`, `initializer_list`, `enum class`, structured bindings, `if constexpr` | |
| **CX7** | C++20: concepts and `requires`, `<=>`, `consteval`/`constinit`, designated initializers, coroutines | |
| **CX8** | libstdc++: its headers, then its sources, compiled by EmbCC | **done** — 193/193 objects on both targets (`make test-libstdcxx`), and the OS's cxxdemo, with `<iostream>`, running on EmbLinkOS |
| **CX9** | C++ on EmbLinkOS: embcc compiling C++ on the metal, over newlib and emlibc | **done** — the kernel's `test embcc cxx`: embcc.elf compiles C++ on the OS, embld.elf links it, it runs |

## Using it

    embcc -c prog.cc -o prog.o          # .cc .cpp .cxx .C .c++, or -x c++
    embcc --emit-c prog.cc              # print the C it lowers to

The object is an ordinary ELF object with g++'s names: link it with
libstdc++ and libsupc++ (the harness: `tests/harness/<arch>/link.sh --cxx`).
`-std=` is accepted and ignored — there is one dialect, growing toward C++20.

## How the lowering works

- `src/cxx/tok.c` lexes the whole unit up front (C++ needs lookahead and
  replay); `parse.c` parses declarations and statements with their
  semantics, `expr.c` expressions — every implicit conversion, reference
  binding and temporary made explicit in the tree — and overload
  resolution; `class.c` lays classes out and builds objects; `vtable.c`
  makes vtables, construction vtables and VTTs; `template.c` instantiates
  templates; `mangle.c` names things as the Itanium ABI does; `emit.c`
  writes C.
- A reference is a pointer in C; a member function takes `this` first; a
  constructor or destructor is two functions, C2/D2 (the base-object body)
  and C1/D1 (the complete object, calling it — with virtual bases, both
  call one body, see below).
- A temporary is a variable declared at its full-expression (a GNU
  statement expression) and destroyed at its end — or, bound to a local
  reference, at the end of that reference's block.
- Every way out of a scope — its end, `return`, `break`, `continue`, `goto`
  — first runs the destructors of what the scope built, newest first.
- A namespace-scope object whose initialization runs code gets it from one
  function per unit, listed in `.init_array`; its destructor is registered
  with `__cxa_atexit`. A function's local static is guarded by
  `__cxa_guard_acquire`/`release`; in an inline function it is a weak
  global under its Itanium name, so every unit shares one.
- Only what is used is emitted: inline functions, implicit special members
  and internal variables through a worklist.
- Every special member the standard declares implicitly exists as a real
  candidate in overload resolution; a trivial one lowers to nothing or a
  struct copy, a non-trivial one gets a memberwise definition on first use.
- A class that is not trivially copyable (a non-trivial copy/move
  constructor or destructor) crosses calls the Itanium way: an argument is
  the address of a temporary the caller builds and destroys after the
  full-expression; a result is built by the callee in a slot the caller
  passes first — before `this` — which C cannot say for aarch64, where the
  slot travels in x8 whatever the size: EmbCC's C has
  `__attribute__((embcc_sret))` on a first parameter for it (x86-64 needs
  nothing: the slot is the first argument, returned in rax). A local
  returned by name from every return is built in that slot (NRVO), as g++
  does, so copies happen exactly where g++'s do.
- Pointers to members are Itanium's: a data member's is its offset (null
  -1), a member function's `{ptr, adj}` (`struct __cx_pmf`), a call
  through one dispatching on an odd ptr as a virtual one will.
- A class with bases or virtual functions is laid out by Itanium's
  algorithm (class.c): the vptr, a primary base sharing it, empty bases at
  offset 0 unless a same-typed subobject is there, a non-POD base's tail
  padding reused. C cannot say that with nested structs, so such a class is
  a packed C struct with every offset spelled out (a base as its bytes),
  and a base is reached by a pointer shifted by its offset (`E_BASE`); a
  trivial copy of a class whose tail padding a derived class may use
  copies its data size only.
- Virtual functions: a vtable group per dynamic class — the primary vtable
  extending its primary base's, one secondary per other dynamic base,
  `this`-adjusting thunks (`_ZThn16_...`) where a secondary's overrider
  lives elsewhere — emitted where the key function is defined, else weak
  in every unit needing it; constructors and destructors store the vptrs;
  a virtual destructor has its deleting D0; typeinfo objects are
  libsupc++'s classes' (`__si_`/`__vmi_class_type_info`), and typeid and
  dynamic_cast use them and `__dynamic_cast`.
- Virtual bases (vtable.c): the complete object's subobjects as a tree, a
  virtual base once; the tables built from it the way g++'s class.cc does
  (vcall and vbase offsets, virtual thunks `_ZTv0_n24_...`, construction
  vtables `_ZTC...`, the VTT `_ZTT...`, the null entries g++ leaves), so
  they come out byte for byte the same. A class with virtual bases has
  one constructor body taking the VTT and whether the object is complete:
  C1 passes its own VTT and builds the virtual bases first (in their
  construction order, with the most derived class's mem-initializers),
  C2 takes its derived class's sub-VTT; destructors mirror it. A virtual
  base is reached through the vbase offset in the object's vtable (an
  `E_BASE` with a virtual step).
- Coroutines (coro.c builds the parts, emit.c lowers them): the frame is a
  C struct beginning as libstdc++'s `coroutine_handle` and g++ expect —
  the resume and destroy functions' addresses (resume null once at the
  final suspend: `done()`), then the promise at its alignment — followed
  by the suspend point, the parameters' copies, `this`, and every local,
  awaiter and temporary of a full-expression that suspends. The function
  itself is the ramp (allocate, copy the parameters, build the promise,
  the return object, run the body to its first suspension); the body is
  one C function a `switch` enters at the suspend point's label, run to
  resume (`destroying` 0) or to destroy (1: at the label, what is alive
  there is destroyed, then the promise, the copies and the frame). A
  `co_await` is taken out of its expression: its awaiter made and, unless
  ready, the point stored, `await_suspend` called and the body returned
  from — before the rest of the expression, which then reads
  `await_resume()`; one in an operand of `?:`, `&&`, `||` runs only when
  that operand would.

## Status

**CX1 done** (September 2026): tests/cxx — `basics`, `classes`, `scopes`,
`globals` — run on both targets and agree with g++, and
`tests/golden/cxx-abi.sh` links an EmbCC half with a g++ half that call
each other (all the builtin types, qualifiers, substitutions, function
pointers, arrays, enums, a nested class, a class by value, `std::`, and
g++ constructing EmbCC's class). The OS's cxxdemo checks 1–3, 5 and 6 pass
on both targets (4 needs templates, CX4).

**CX2 done** (September 2026): operator overloading (members and
non-members; `[]`, `()`, `->`, `++`/`--` in both forms, compound
assignment, operators on enums), argument-dependent lookup for operators
and calls, conversion functions (including `explicit operator bool` in
conditions) and converting constructors, copy and move — user-written and
implicit memberwise, with implicit move on `return local;` and NRVO —
classes that are not trivially copyable passed and returned by value,
pointers to members, and class-specific `operator new`/`delete`.
tests/cxx `operators`, `copymove` (every copy, move and destruction
logged: the same as g++'s), `memptr`, `alloc` agree with g++ on both
targets, and cxx-abi now passes a non-trivially-copyable class by value
and returns it through the slot both ways across the EmbCC/g++ boundary
(x8 on aarch64), with operators, a conversion function and member
pointers.

Already there ahead of their milestones: `enum class` and fixed underlying
types, `auto` and `decltype` for variables, `if`/`switch` with an
initializer, `static_assert`, delegating constructors, default member
initializers.

**CX3 done** (September 2026): single and multiple inheritance with g++'s
layouts, virtual functions (overriding with and without `virtual`,
`override`/`final`, pure virtuals and abstract classes, virtual
destructors and deleting destructors, thunks), `typeid` and
`dynamic_cast` (down, across, to `void *`, failing, of references).
tests/cxx `inherit` and `rtti` agree with g++ on both targets, and
cxx-abi splits one hierarchy across the two compilers — each calling the
other's virtual functions, g++'s dynamic_cast reading EmbCC's typeinfo,
deleting through a thunk to EmbCC's D0, layouts compared number for
number.

**CX3b done** (September 2026): virtual base classes — layout after the
non-virtual part, nearly empty virtual bases sharing the vptr (with g++'s
"lost" primaries: the interface pattern), vbase and vcall offsets, virtual
thunks, construction vtables and VTTs (virtual VTTs for virtual bases with
their own), constructors and destructors taking the VTT, virtual bases
built first by the most derived class, conversions and member access
through the vtable, pointers to their members, copies and assignment. For
every hierarchy tried (diamonds, virtual bases without virtual functions,
interfaces, a virtual base's own virtual base, construction-time virtual
calls) every vtable, construction vtable, VTT and typeinfo g++ emits is
byte for byte what EmbCC emits (compared symbol by symbol, relocations
included). tests/cxx `vbases` agrees with g++ on both targets, and
cxx-abi now splits a virtual-base hierarchy across the compilers the
hard way: EmbCC's `Join : Left, Right` builds g++'s `Right` through a VTT
entry pointing into EmbCC's construction vtable, g++'s `Join2 : Right,
Left` builds EmbCC's `Left` the same way, and each destroys through the
other's destructors, casts down from the virtual base with the other's
typeinfo, and agrees on the layout.

Found on the way: a pointer to a base's member did not apply to a derived
object nor convert to the derived class's (both done, a conversion
through a virtual base refused); a nested implicit constructor reset the
flag that lets an abstract class be built as a base; `cx_fmt` cut its
result at 1024 bytes; and EmbCC's C could not take a member of a struct
returned by value (`f().m`), which vtable.c uses — tests/exec
`struct-rvalue-member` now runs it at every level on both targets.

Already there ahead of their milestones: `enum class` and fixed underlying
types, `auto` and `decltype` for variables, `if`/`switch` with an
initializer, `static_assert`, delegating constructors, default member
initializers.

Refused until later, each naming its milestone: lambdas, range-`for`,
`initializer_list`, deduced return types (CX6); coroutines (CX7). Access control
is parsed but not yet enforced; anonymous struct/union members, bit-fields
in a class with bases or virtual functions, and copying arrays of
non-trivially copyable objects are not supported yet.

**CX4 done**: class, function, member, alias and variable templates by
token replay (src/cxx/template.c) — a template's tokens are kept and each
instance parses them again with the parameters bound, so an instance is
ordinary C++ to the rest of the front-end; a function template's
declaration is also read once as a pattern (CT_TPARAM, CT_TID, CT_DEP) for
deduction and mangling. Class instances are defined when first needed
complete, their members when first used (or from their out-of-class
definitions); deduction through T, T*, const T&, T&& (forwarding),
T(&)[N], A<T> (and its bases) and TT<T> (template template parameters);
explicit arguments; explicit and partial specializations — matched
exactly, the most specialized of several chosen (13.7.6.2); partial
ordering of function templates; non-templates preferred on ties; SFINAE
(a substitution error unwinds to the attempt and drops the candidate);
explicit instantiation and `extern template`. A dependent expression in a
pattern is read into its Itanium mangling (`IXsr6is_ptrIT_E5valueE`), types
inside it joining the substitution table and parameters used as operands
not (`XT_E`, `sZT_`).

Variadic templates: a bound pack is a CS_PACK symbol — a template
parameter pack's arguments, or a function parameter pack's parameters —
and an expansion re-reads its pattern once per element with a stack
(`pack_push`) making lookup of the pack's name find the current element;
so `f(g<Ts>(args)...)`, `{sizeof(Ts)...}`, `tuple<Ts...>`, `Ts... args`,
`: Bases(args)...`, `struct M : Bases...` are all the ordinary parser
reading ordinary C++ N times. The innermost `...` expands every pack in its
pattern; `sizeof...` counts one. In a pattern, `Ts...` stays one argument
(`expansion`) that deduces a pack element by element — from a call's
trailing arguments, from `tuple<T, Rest...>` against `tuple<int, char>`
(classes' own packs flattened), from `seq<Is...>` values. Fold expressions
in all four forms, empty `&&`/`||`/`,` folds giving `true`/`false`/`void()`.
Mangled as g++ does: `DpT_` parameter packs (a substitution candidate as a
whole), `J...E` argument packs, `XspT_E` value-pack expansions.

Found on the way: `T x(A(5))` was parsed as a function declaration (a
parameter list is now tried and, failing, it is an initializer);
mem-initializers named bases by name, which two instances of one template
share (`tuple<Rest...>(r...)` inside `tuple<T, Rest...>`) — a mem-initializer
naming a type now resolves to the class; a call returning a not yet
instantiated class, `delete` of one and operator lookup on one instantiate
it first; `&"literal"` (binding `const char(&)[N]`) is written as a cast
the C side accepts.

Not yet: `decltype(...)::` as a qualifier; a nested expansion of a pack an
enclosing expansion is iterating, within one list element (`f(g(xs,
xs...)...)`); function parameter packs named in a dependent signature
(`sZfp_`).

Not yet with virtual bases: covariant return types that need a thunk to
adjust the result.

**CX5 done** (September 2026): exceptions, interchangeable with g++'s,
built in three layers.

1. *Unwind tables* (debug/eh.c): the code generators record what each
   prologue did and every function gets a CIE/FDE in `.eh_frame` — always
   for C++, for C on `-funwind-tables` — so libgcc's unwinder can cross
   EmbCC's frames (tests/golden/unwind-through.sh: g++'s exception through
   EmbCC's C at -O0 and -O2, the catcher's callee-saved registers intact).
2. *Landing pads in EmbCC's C*: `__builtin_eh_region { } __builtin_eh_landing
   (exc, sel, actions) { }` and `__builtin_eh_typeid`, which C++'s lowering
   writes; irgen keeps regions as instruction ranges, both code generators
   record calls and pads, and eh.c writes the LSDA `__gxx_personality_v0`
   reads (tests/golden/eh-regions.sh).
3. *C++*: `throw` (`__cxa_allocate_exception`/`__cxa_throw` with the type's
   typeinfo — pointers to classes get their `__pointer_type_info` — and
   destructor), `throw;`, `try`/`catch` by value, reference, pointer and
   `...` (the pad dispatching on the selector; a handler is a block whose
   cleanup is `__cxa_end_catch`), and every destructor a throw must run:
   locals (a region opened as each is built, closed at its scope's end,
   its pad destroying it and handing on — by `goto` to the enclosing pad,
   or `_Unwind_Resume` from the outermost), temporaries (flagged as built),
   a constructor's finished bases and members, `new`'s storage.
   `noexcept` functions and destructors get a catch-all region calling
   `__cxa_call_terminate`. `-fno-exceptions` turns it all off.

tests/cxx `except` agrees with g++ on both targets; cxx-abi throws each
way across the compilers (EmbCC's exception type caught by g++'s code,
g++'s by EmbCC's, destructors of both sides' frames run);
tests/golden/cxx-noexcept.sh ends in std::terminate as it must.

Also: function-try-blocks (a constructor's or destructor's handler
rethrows at its end, after the bases and members are destroyed), the
`noexcept` operator (a call is potentially throwing unless its function is
noexcept, a destructor, or an implicit special member whose parts'
counterparts are not — so `move_if_noexcept` chooses as g++'s does),
arrays of objects and `new T[n]` destroyed element by element when one
throws, and a local static retried after its initializer throws
(`__cxa_guard_abort`).

Optimization: a function whose landing pads guard a call that can throw is
compiled in the plain memory model — the optimizer and register allocation
do not yet model the edges from calls into pads. Calls of functions that
cannot throw (noexcept ones, destructors; their prototypes say
`__attribute__((nothrow))` to EmbCC's C) need no pad, and a function none
of whose guarded calls can throw has no pads at all and is optimized as
any other.

**CX6 in progress.** Part 1: range-`for` (arrays, `begin()`/`end()`
members or found by ADL, a temporary range kept alive for the loop),
deduced return types (`auto`, `auto &`, `const auto &`; a body read on the
first call when its type is needed — a member used before its class's
delayed bodies are read, a template's instance), and lambdas. A lambda is
a class of its own (a local class: internal names, as are the instances of
templates over it) whose members are its captures — explicit, init-captures
and the ones a capture-default makes as the body names them — and whose
`operator()` the body defines; `this` is a member too (`__cx_this`), so
the enclosing object's members are reached through it. A lambda inside a
lambda that names the enclosing one's capture captures it again (by copy
or by reference to that member). A lambda capturing nothing converts to a
pointer to function (a static `__cx_invoke` calling `operator()` on an
empty closure). Generic lambdas: each `auto` parameter is an invented
template parameter (`__cx_autoN`, which the tokens are rewritten to once,
so `auto...` is an ordinary pack), `operator()` a member template replayed
per call; since its body is read only then, after the closure is complete,
what a capture-default captures is decided before from the body's tokens
(a local its names find where the lambda is; `this` if it or a member is
named) — possibly more than the standard's odr-use rule would, never less.
tests/cxx `lambdas` and `rangefor` agree with g++ on both targets.

Found on the way: a template instance over a local class had a name with
linkage (`6._anon`); a function returning a pointer to function
(`int (*f(int))(int)`) is written through a typedef, which EmbCC's C needs;
a braced list already given its type recursed forever in conversions.

Part 2: constant evaluation (src/cxx/consteval.c). Where a constant is
needed — `static_assert`, an array bound, a template argument, an
enumerator, a case label, `if constexpr`, a const variable's value — an
expression the folder cannot reduce is run by an interpreter over the
front-end's own trees: objects are byte blocks laid out as the target lays
them out (members, bases and elements are offsets), a pointer is a block
and an offset (stored in an object as a handle, so copies keep pointing
where they did), each call's locals get blocks of their own. Loops,
`switch`, recursion, references, member functions, constexpr constructors
(bases and mem-initializers), aggregates, strings, lambdas, floating
point; anything else (a non-constant variable, a function that is not
constexpr, out-of-bounds access, division by zero, 20M steps) makes the
expression not a constant, quietly — the caller reports it if it needed
one. emit.c keeps the plain folder, so run-time code still calls constexpr
functions. `__builtin_is_constant_evaluated()` is true in the interpreter,
false at run time. `if constexpr` skips the discarded branch unread.

Also: `decltype(auto)` (variables and returns; mangled `Dc`); structured
bindings to an array's elements, a class's data members and a tuple-like
class's parts (`std::tuple_size`, `std::tuple_element`, `get<i>` as a
member template or by ADL), in declarations and range-`for`, visible to
lambdas; `std::initializer_list` — a braced list converted for an
argument (its backing array a temporary of the full-expression) or a
variable (a hidden local, living as long; also a range-`for` over a braced
list), `auto x = {...}`, initializer-list constructors tried first in
list-initialization, `initializer_list<T>` deduced from a braced list, the
list-to-`initializer_list` conversion preferred in overload resolution;
user-defined literals (cooked, raw, `template<char...>`, strings with their
length; mangled `li<suffix>` as g++ does) — the lexer keeps a C++
literal's suffix, and reads digit separators (`1'000`) and binary literals
(`0b101`, C too); `[*this]`. tests/cxx `constexpr`, `bindings`,
`initlist`, `udl` agree with g++ on both targets.

**CX8 started** (libstdc++'s headers, compiled by EmbCC). The
preprocessor answers C++'s feature tests — `__has_builtin` (the C++
front-end says which builtins and trait intrinsics it has; also when a
macro's expansion produced it, as `_GLIBCXX_HAS_BUILTIN` does),
`__has_include(_next)`, `__has_attribute`, `__has_cpp_attribute`, each
"defined" to `#ifdef` — and defines the `__cpp_*` macros of the features
implemented (`__cpp_exceptions`/`__EXCEPTIONS` unless -fno-exceptions,
`__cpp_rtti`, `__GNUG__`); C units see none of it. The C++ predefined
table no longer claims `__float128`, `__float80` or the extended floating
types (tools/gen-predef.sh), which libstdc++ would otherwise use;
`__int128` it does claim (`__SIZEOF_INT128__`, and libstdc++'s
`__GLIBCXX_TYPE_INT_N_0` outside strict modes, as g++ defines them), now
that EmbCC has it. tests/cxx `features`.

Type-trait intrinsics (src/cxx/traits.c), g++'s names: the class
categories and properties (`__is_class`, `__is_empty`, `__is_polymorphic`,
`__is_final`, `__is_trivial`, `__is_pod`, `__is_standard_layout`, ...),
`__is_same`, `__is_base_of`, the constructible / assignable / convertible
families (built as the expressions std::declval would give, a failure a
substitution failure), `__underlying_type` and the type transforms
`<type_traits>` asks `__has_builtin` about. Pattern fixes on the way:
`T C::*` with C a template parameter; an attribute between `struct` and
a class template's name; default template arguments ending in `>>`; an
alias template instantiated in the middle of a `>>`; decltype of a
dependent expression in a declaration (a dependent type, mangled
`DT...E`); `bool(...)` and `f<args>` in dependent expressions (a
one-argument functional cast mangled `cv <type> <expr>` as the ABI says).
C++ errors now end with the instantiations under way ("note: in the
instantiation of ..."). tests/cxx `traits`.

Concepts' core (from CX7), which C++20-mode libstdc++ uses unconditionally
(src/cxx/concepts.c): concept definitions; requires-clauses after a
template head and trailing (a class template's member whose clause fails
in an instance is no candidate, its body never read); requires-expressions
(simple, type, compound with `noexcept` and `-> C<...>`, nested
requirements, parameters); type-constraints (`template<C T>`) and
constrained placeholders (`C auto`). A constraint is kept as tokens and
decided by replaying them with the arguments bound — each operand of `&&`
and `||` on its own, a substitution failure making that operand false (so
`||` works as the standard says); a concept's answer is cached per
arguments. Partial specializations are chosen by their constraints too,
the constrained one winning a tie with an unconstrained one (subsumption
beyond that is not decided). `__cpp_concepts` stays undefined for now,
so libstdc++ keeps its concept-based library parts (ranges, <concepts>)
off. tests/cxx `concepts`.

libstdc++'s headers, continued: `<type_traits>`, `<utility>`, `<tuple>`,
`<new>`, `<limits>`, `<exception>`, `<typeinfo>`, `<initializer_list>`,
`<cstddef>`, `<cstdint>`, `<cstring>`, `<cstdlib>`, `<cwchar>`, `<cmath>`
and `<algorithm>` compile with EmbCC. On the way: variable templates'
explicit and partial specializations; defaults on a class template's
forward declaration kept by its definition; explicit specializations
declared before being defined; friend class templates; constructor
templates were registered as ordinary functions (hiding the
injected-class-name) — now constructors; using-declarations join
overload sets (entries standing for the named functions); aliases
declared with an attribute; function parameters in scope in a trailing
return type (mangled `fp_`, `fp0_`, ...); a parenthesized dependent
`X<T>::v && ...` no longer taken for a cast; default template arguments
converted to their parameter's type; the library builtins
(`__builtin_memchr`, `__builtin_acosf`, ...) become calls of the C
functions, and the floating classifications (`__builtin_isnan`,
`__builtin_signbit`, `__builtin_fpclassify`, ...) C expressions, EmbCC's C
knowing neither; stddef.h's `max_align_t` for C++. tests/cxx
`headerforms`.

Then `<vector>`, `<string>`, `<map>`, `<set>`, `<deque>`, `<array>`,
`<sstream>`, `<numeric>` and `<iostream>` compile too. On the way:
anonymous unions and structs in classes (their members named through
them, `offsetof` too) — basic_string's buffer; member classes of a class
template defined outside it (`class A<T>::B { }`); out-of-class member
templates of a class (constructors too: they were being registered as
functions) and their template parameters kept visible inside the class's
scope; out-of-class definitions of static members initialized in the
class; `>>` inside a skipped declaration (a namespace's `}` was being
swallowed); explicit specializations of variable templates and partial
ones; defaults of a class template's forward declaration; friend class
templates and template-ids in elaborated specifiers; constructor templates
beside the injected-class-name; a partial specialization matched through
`void_t<typename T::x>` — deduced, then its pattern substituted and
compared (the dependent part a non-deduced context till then); default
template arguments as substitution failures in deduction (`typename =
_RequireInputIter<It>`); packs deduced from a function type's parameters
and reconciled with a function parameter pack; unevaluated operands
(decltype, sizeof, noexcept, requires) no longer instantiate function
bodies (std::declval's body is an error); pack expansions in new's
arguments; pseudo-destructor calls; `static_assert` on a class with
`operator bool`; value-initialized pointers are null pointers; copy-
initialization from another class through its conversion function;
`if (wchar_t('0') == L'0')` and `(less<T>()(a, b) || ...)` no longer
declarations or casts; `typename X<T>::type = v` a value template
parameter; `X<T>::template f<U>()` a call; statement attributes
(`[[likely]]`); the same user conversion ranked by its second part
(`operator[](const K&)` vs `(K&&)`); the GNU `__atomic_*`/`__sync_*` and
`__builtin_*_overflow` builtins; `__cpp_constexpr_dynamic_alloc` (C++20
libstdc++ declares `std::construct_at` only with it); in C++ units
`va_list` is `__builtin_va_list` and newlib's `__VALIST` follows, so
`vsnprintf` matches libstdc++'s calls. tests/cxx `headerforms2`.

A first program runs against the library: tests/libstdcxx/hello.cc
(`std::vector`, `std::string`, `std::sort`, `std::cout`, a sorted
`vector<string>`) is compiled by EmbCC with libstdc++'s headers, linked
with g++'s libstdc++.a, and prints what g++'s build prints, on both
targets (tests/golden/cxx-libstdcxx.sh; skipped where the cross libstdc++
is not installed). That needs EmbCC's names to be g++'s: the Itanium
standard abbreviations (`Sa`, `Sb`, `Ss`, `Si`, `So`, `Sd`, also for the
scopes of members of those classes); and C++ units now present
themselves as g++ 16 (`__GNUC__`, `__GNUC_MINOR__`), newlib's headers
then taking their GNU branches (`__func__` was NULL without it; C units
unchanged). `<memory>` and `<optional>` compile too. On the way: partial
ordering of function templates deduces each parameter pair on its own
and exactly (and counts an operator's implicit object parameter); `&f`
of an overloaded function template deduced from the target type; inline
members of an `extern template` class instantiated when used; default
arguments merged across redeclarations; default arguments in patterns
kept as tokens till used (`_Alloc()` read too early); `(throw (E()))` is
no cast; `__func__`, `__FUNCTION__`, `__PRETTY_FUNCTION__`; the lock-free
queries of `<atomic>` folded; an instantiated body leaves an unevaluated
operand's context; the preprocessor takes `M()` as one empty argument of
a one-parameter macro.

A second program runs the same way: tests/libstdcxx/map.cc (std::map
with string and int keys — `operator[]`'s piecewise construction through
tuples, a braced `insert`, ordered iteration, structured bindings).
`<functional>`, `<fstream>`, `<variant>`, `<thread>`, `<mutex>`,
`<condition_variable>`, `<future>` and `<shared_mutex>` compile too. On the
way: a value template parameter whose type is dependent (`typename
enable_if<C, bool>::type = true`) is substituted during deduction, a
failure dropping the candidate (pair's implicit and explicit constructors
were both viable); deduction sees through alias templates (the alias's own
parameters deduced through its pattern, then the written arguments from
those — `index_sequence<I...>`); a base class template is deduced from
each of a class's bases that are instances of it (tuple's `_Tuple_impl<I,
...>`); member templates of class templates defined outside the class, and
their out-of-class destructors; pack expansions in a variable's
parenthesized initializer; Clang's `__make_integer_seq` (libstdc++ asks
`__has_builtin` and builds its index sequences with it; the classes, and
so the ABI, are g++'s); fold-expressions in dependent template arguments;
partial ordering that sees `__void_t<...>` as `void`; a call's explicit
template arguments no longer leak into the calls resolved while deducing
it (they dropped every non-template constructor there); conversions to an
instance not yet instantiated; a converting constructor taking its class
by value; a defaulted constructor first needed in an unevaluated operand
still instantiates what it calls; the primary template's out-of-class
static members not defined for an explicit specialization, and a
template's static data members weak, as inline variables are — whose
dynamic initialization is now guarded by Itanium's `_ZGV` variable (each
unit that defines one initializes it once). The ambiguity error lists the
two candidates. tests/cxx `templates3`.

The C front end, on the way (the C++ front end emits it for
`std::forward<const char (&)[N]>`): declarators nested in parentheses
around a function declarator — functions returning pointers to functions
or to arrays, `void (*signal(int, void (*)(int)))(int)` — declared and
defined; the declarator reader is now general (a parenthesized declarator
read after the suffixes that bind tighter). tests/exec `nested-decl`.

**CX7 started**: three-way comparison, and `__cpp_concepts` (201907L) and
`__cpp_impl_three_way_comparison` (201907L) defined — so libstdc++'s
concept-based parts are on: `<concepts>`, `<ranges>`, `<compare>`, and
the library's own `operator<=>` for its containers, strings, pairs and
tuples. The built-in `<=>` (E_CMP3: each operand once; integers, enums
and pointers give `std::strong_ordering`, floating types
`std::partial_ordering`, unordered with a NaN — the category object's
byte, as `<compare>` lays it out; constant-evaluated too); the rewritten
candidates when no operator is written for a comparison: `x @ y` as `(x
<=> y) @ 0` or `0 @ (y <=> x)`, `x != y` as `!(x == y)`, `x == y`
reversed (a synthesized 0 is a null pointer constant, as `<compare>`'s
`__literal_zero` wants); defaulted `==` and `<=>` (bases then members;
an `auto` `<=>` returns the weakest member category) and defaulted
relational operators (through `<=>`); `__builtin_bit_cast(T, e)`, the
only defined way to reinterpret an object's bytes -- `*(int *)&f` breaks
strict aliasing, a union is defined in C and merely conventional in
C++, and memcpy cannot appear in a constant expression; a function
parameter PACK visible in a trailing return type, so `-> decltype(f(a...))`
-- the shape every forwarding wrapper in the standard library is written
in -- deduces (it worked only when a TYPE pack happened to appear in the
expansion too, as `forward<A>(a)...`, which is why it had gone
unnoticed); `typeid` of a function type, an enum, an array, a member
pointer and a pointer to any of them, each emitted as the ABI class the
Itanium ABI names for that shape; a repeated default template argument
DIAGNOSED (13.2p12) rather than silently accepted, which is a missing
refusal and was hiding a real mistake in this tree's own `<iosfwd>`; a
conversion function inherited
from a base considered when an operator's built-in candidates are built,
so `a == 7` works on a class whose `operator int()` is in a base; a base
member brought in by a using-declaration given the DERIVED class's
implicit object parameter ([over.match.funcs]/4), without which
`a = 3` was ambiguous against the implicit copy assignment; a nested
braced list offered to an
initializer-list constructor as one argument before its elements are
offered one by one, so `vector<vector<int>> v{{1, 2, 3}}` is accepted
(overload resolution skipped the first step, and rejected the call
whenever no constructor happened to take that many arguments); an
unnamed local type numbered per enclosing FUNCTION rather than per
block, so two lambdas in two different `{ }` no longer mangle alike —
invisible until one reaches a template argument, where it made two
distinct instantiations collide; a defaulted `operator<=>`
implicitly declaring `operator==` with the same access
([class.compare.default]/2) — without which `a != b`, which rewrites
through `==` and never through `<=>`, did not compile for a class whose
only comparison was the defaulted `<=>`; `operator<=>` declared as a
friend specialization. On the way, for the library: a static `operator()`
(C++23, which g++ and libstdc++ use in C++20); hidden friend function
templates (their declarations read in their class, an instance's
arguments bound); a trailing requires-clause sees the function's
parameters; a parenthesized fold in a constraint is one atomic constraint;
C++20 template lambdas (`[]<class T>(T x)`, `[]<int... I>(...)`, mixed
with `auto` parameters); aggregate initialization of classes with bases
(C++17: the bases first); implicit conversions through constructor
templates (`int` to `__max_size_type`), without a second user conversion
for the copy constructor's argument; conversion functions inherited from
bases, and explicit ones in direct-initialization (`unsigned{day}`);
`explicit(cond)` with a class constant, and decided per specialization in
a member template; conversion functions to different types distinct;
scoped enumerators visible in their own list; `E{}`/`E{n}` for enums;
class instantiation re-entered while choosing its partial specialization
(its constraints named it) no longer defines it twice; unnamed classes and
enums mangled `Ut_`, `Ut0_`, ... as g++ does (they were `._anon`, not an
identifier); a definition of a friend-declared function template reads
its names where it is defined; defaulted comparisons are defined, not
given an empty body; the constant evaluator bounds its nesting.
tests/cxx `spaceship`; tests/libstdcxx/compare.cc (vector, string, pair
and tuple `<=>`, a defaulted `<=>` over a string member, `std::sort`
through the rewritten `<`) matches g++'s build on both targets.

Class template argument deduction (C++17, with C++20's aggregates;
`__cpp_deduction_guides` defined, so libstdc++'s guides are declared): a
class template's name alone before a declarator, or before `(` / `{` in
an expression, stands for the class its arguments are deduced for from
the initializer. The candidates are function templates made of the
deduction guides (their parameter lists and result types kept as tokens,
read with the deduced arguments bound), of the primary template's
constructors (its body scanned for them, and for the member typedefs
their parameters name — constructor templates' parameters after the
class's), and of an aggregate's members; the copy deduction candidate
first; for a braced list, initializer-list constructors first with the
list one argument. Each is deduced as a call's template arguments are,
the viable ones compared by their substituted parameters (explicit ones
not for copy-initialization, a guide winning a tie). Inheriting
constructors (`using B::B;`): each base constructor but the default, copy
and move ones becomes the class's (unless it declares one like it), its
body the base built from the forwarded arguments — constructor templates
wrapped per specialization, through as many levels as inherited them
(`std::optional`'s storage). A class-type argument sliced to a base
parameter by value (`random_access_iterator_tag` as
`forward_iterator_tag`) now copies the base part. tests/cxx `ctad`;
tests/libstdcxx/ctad.cc (pair, tuple, vector from a list and a range,
array, optional, map from a list of pairs) matches g++ on both targets.

Designated initializers (C++20: members named in declaration order, the
rest from their default member initializers or value-initialized) and
`using enum` (`__cpp_using_enum`). For the library: explicit
specializations of an instance's static data members (declared here,
defined by the library — `__timepunct_cache<char>::_S_timezones`);
mem-initializers naming an anonymous union's members (`_M_dummy()`,
`_M_loc(l)`); member classes of a class template's instance defined only
when first needed (`vector<T>`'s helpers hold a `T`, and
`pmr::vector<_BigBlock>` is declared while `_BigBlock` is incomplete);
using-declarations with pack expansions (`using _Bases::_S_fun...;`,
std::variant's converting constructor) and several declarators; inheriting
constructors named through an alias (`using _Base::_Base;`); out-of-class
members of partial specializations (`_Map_base<...>::operator[]`, their
own parameters bound; a replay into another class skipped) and of member
class templates (`any::_Manager_internal<T>::_S_manage`); instance members
called before their out-of-class definition is read, defined at the end
of the unit (the point of instantiation); a qualified friend template
declares nothing (it made a second `__get`); partial ordering that
requires a parameter deduced from two pairs to agree, and that ranks a
parameter pack below a single parameter; alias template-ids deduced and
ordered in partial specializations (`index_sequence<I...>`); a fold
whose operand names a type pack in template arguments is no cast; member
operators found in bases (`hash<string>`'s `operator()`); a class
constant as a value template argument (`__and_<...>{}`); explicit
conversion functions in direct-initialization (`iterator(__loc)`), and
a derived argument sliced to a base by the copy constructor (before any
parenthesized aggregate initialization); `sizeof(member)` in a class
without an object; `<` after a value in a default template argument is
less-than; a redeclaration of a defined explicit specialization. With
these `<list>`, `<any>`, `<string_view>`, `<random>`, `<iomanip>`,
`<locale>`, `<regex>` and `<memory_resource>` compile. tests/cxx
`cxx20misc`; tests/libstdcxx/containers.cc (list, deque, set,
unordered_map, string_view, any, variant, mt19937 with a fixed seed,
iomanip, designated initializers) matches g++'s build on both targets.

`<format>` and `<chrono>` (with its calendar) compile and run —
`std::format` with widths, fill, precision and bases, durations,
`year_month_day` arithmetic — and `<climits>`: EmbCC now ships a GCC-style
`include/limits.h` (newlib's chains to it with `#include_next` in C++
units, and it chains to newlib's when found first, as in the OS build; it
only defines what the C library did not). On the way: a class whose only
bases are empty (at offset 0, no vptr, no member displaced) is laid out
as the plain struct of its members, bit-fields included (`_Spec :
_SpecBase`); partial specializations written with fewer arguments than
the template has (`__common_ref_impl<X&, Y&&>` of a `<A, B, class =
void>`) match when the rest are the defaults; a template template
parameter in a partial specialization's pattern is deduced
(`__is_specialization_of<C<A...>, C>`); a parameter whose template
parameters sit only in non-deduced contexts (`type_identity_t<C>`,
`format_string<Args...>`) leaves the argument to its conversion; a
written template-id's defaulted arguments are not compared
(`basic_string_view<C>` against `basic_string_view<char, traits>`);
`decltype(e)::type`; `switch` on a scoped enum; `__remove_cv` of an array
removes its elements' cv; a block's using-declaration keeps
argument-dependent lookup; `&&`-qualified members chosen for an rvalue
object; an explicit instantiation (declaration) of one member of an
instance; template arguments of a declarator naming one of several
function templates read as written; default arguments of a function
template's first declaration kept by its later definition; member
template definitions with a requires-clause; the functions a constructed
instance's vtables name are instantiated at the end of the unit;
`__builtin_alloca` (in EmbCC's C too: an `IR_ALLOCA` alive until the
function returns). tests/cxx `cxx20misc2`, tests/exec `alloca`;
tests/libstdcxx/format.cc matches g++'s build on both targets.

GNU complex types (`__complex__ float/double/long double`, CT_COMPLEX,
lowered to C's `_Complex`, mangled `C<type>`): arithmetic with complex
and real operands, `==`/`!=`, unary `-`/`+`, compound assignment,
`__real__`/`__imag__` as values and lvalues (E_CPART), `{re, im}`
construction, conversions between element types and from reals, and the
`<complex.h>` builtins (`__builtin_cabs`, `csqrt`, `cexp`, `cpow`, ... as
newlib's functions). With them `<complex>` compiles; tests/cxx `complex`;
tests/libstdcxx/complex.cc (arithmetic, abs/arg/norm/conj, sqrt, exp,
pow, float/double conversions) matches g++'s build on both targets. Every
libstdc++ header now compiles except `<coroutine>` (since: below).

The OS's `user/tests/cxxdemo/cxxdemo.cc` (global constructors, new/delete,
templates, local statics, destructors at exit, `<string>`, `<vector>`,
`<iostream>`) compiles with EmbCC for both targets under the OS's flags
and links against its crt0/syscalls and libstdc++ (CX8's proof; it now
runs on the OS too — below).

**Coroutines** (CX7's last core feature; `__cpp_impl_coroutine`, so
`<coroutine>` compiles — every libstdc++ header now does): `co_await`,
`co_yield`, `co_return`; the promise from `std::coroutine_traits` (a
specialization can make a void function a coroutine) built from the
parameters when a constructor takes them; the frame from the promise's
own `operator new`/`delete` when it has them (nothrow, and
`get_return_object_on_allocation_failure`, when that is declared);
`await_suspend` returning void, bool or a handle to resume (symmetric
transfer); `operator co_await` (member or not), `await_transform`;
initial and final suspends; exceptions to `unhandled_exception`, or to the
resumer; `__builtin_coro_done/resume/destroy/promise`, laid out as g++'s.
tests/cxx `coroutines` (a hand-written `std::coroutine_handle`, g++
agrees); tests/libstdcxx `coroutine` and `coroutine2` (generators, lazy
tasks chained by symmetric transfer, exceptions, early destruction,
template/member/lambda/generic-lambda coroutines, `co_await` in `?:`,
`&&`, `||`, loops, a `switch`, a range-for, structured bindings)
match g++'s builds on both targets, at -O2 too.

On the way: `?:` between different class types (one converted to the
other; a derived and a base lvalue giving the base lvalue); `x = {}`
choosing the move assignment (two list conversions to the same class are
compared by their reference binding); a lambda's trailing return type
naming its parameters; `template<> struct ns::T<...>` from outside the
namespace; a class template's conversion function defined outside it; an
explicit instantiation defining the members defined outside the class; a
deleted assignment refused even when the class's assignment would be a
byte copy (a move constructor declared). Two mangling fixes, found against
g++: a conversion function's type may use the substitutions of the name
before it (`cvS_IvE`), and a name in `std` that begins with a
substitution has no `St` before it (`St6HolderIiES_IcE`, as two
specializations of one std template in one signature) —
tests/golden/cxx-abi.sh checks both across the compilers. tests/cxx
`cxx20misc3`, tests/golden/cxx-reject.sh (deleted functions, coroutine
misuse).

`<ranges>` and `<functional>` run: iota views (`__max_size_type`'s
conversions), filter, transform, reverse, take — called, partially
applied and piped, and composed — `ranges::size`, `find_if`, `sort`,
`count_if`; `std::bind` with placeholders, `bind_front`, `not_fn`
(tests/libstdcxx/ranges.cc matches g++'s build on both targets). What
they needed:
- conversion function templates (`template<class T> operator T()`, their
  arguments deduced from the target type, 13.10.3.4; a non-template
  wins a tie)
- explicit object parameters (C++23 `this Self &&self`), which libstdc++
  uses whenever the compiler says it is g++ 14 or later: the object the
  first argument, deduced from; mangled `NH...` as g++ does
- constraint subsumption (13.5.4-5): constraints normalized into atoms
  (a concept-id replaced by its definition, its arguments bound; an atom
  is its expression with those arguments), P subsuming Q when each
  disjunctive clause of P has an atom in each conjunctive clause of Q —
  ordering partial specializations (`_CachedPosition<forward_range>` and
  `<random_access_range>`), function templates, and an instance's
  members differing only in their requires-clause (which are now two
  functions, not a redeclaration)
- a member's requires-clause checked when the member is a candidate, not
  when its class template is instantiated — view_interface<D>'s, while D
  is incomplete — except the special members', whose eligibility makes
  the class trivial or not
- a conjunction's right operand is not checked when its left one fails,
  a disjunction's when its left one holds (13.5.2.2): checking it could
  instantiate what is being decided (`iterator_traits<X>` while choosing
  `__iterator_traits<X>`'s specialization)
- argument-dependent lookup also in the namespaces of a class's bases and
  of a specialization's type arguments (`v | views::reverse`: operator|
  is `_RangeAdaptorClosure`'s namespace's), finding friends declared only
  in a class (which ordinary lookup no longer finds: `ranges::iter_swap`
  is the customization point object, not `filter_view`'s iterator's
  friend), and never for a qualified name
- a default member initializer of an instance read only when a
  constructor uses it (`_Vp _M_base = _Vp();` of a view with no default
  constructor)
- a function bound to a reference to its type, or to a const reference to
  a function pointer (a temporary); a parameter with no template
  parameter in it left out of deduction (13.10.2.1)
- `if constexpr (...) static_assert(...);` (a statement that declares only
  is a null statement)
- `[[no_unique_address]]`: a member of an empty class takes no room (at
  offset 0 unless a subobject of its type is there, as an empty base),
  one of another class lends its tail padding, and a class whose members
  are all such is empty — so std::tuple of empty types, and what is built
  on it, is laid out as g++ lays it out (`unordered_map<int, int>` was 64
  bytes, libstdc++'s 56); such a member has no C field (the class is
  written with explicit offsets, reached by its offset); an empty class's
  trivial copy writes nothing (its byte may be another object's)
- EmbCC's C accepts `f(...)` with no named parameter (C23; C++'s)

tests/cxx/cxx20misc4.cc (g++ agrees on both targets).

Most of the rest of libstdc++ runs (tests/libstdcxx/utilities.cc and
library.cc, each matching g++'s build on both targets): variant and visit,
shared/weak/unique_ptr, std::function, string streams, to_chars/
from_chars, span, <bit>, optional, any, the containers and adaptors,
tuple/apply, string_view; algorithms with back_inserter and
ostream_iterator, <iomanip>, <random>, <regex>, bitset, valarray,
exceptions from vector::at, error codes, std::pmr, atomics,
std::source_location, <numbers>. What it took:
- a union's destructor leaves its members alone (it cannot know which is
  alive: std::variant's storage destroyed a string never built)
- a namespace alias in a block; an alias template's requires-clause
  (substitution failure when it does not hold: `v << x` on an lvalue
  stream no longer finds the rvalue inserter)
- a member function and a member template of the same signature are two
  functions (bitset::to_string)
- a partial specialization's members defined outside it are not the
  primary template's (vector<bool, A>::_M_erase for vector<char>)
- a member function whose address is taken is instantiated
  (regex's _Scanner::_M_eat_escape_ecma); a class built only through a
  constructor template gets its vtable's functions
  (_Sp_counted_ptr_inplace)
- a parameter whose template parameters are all given explicitly takes
  its argument by conversion, not deduction (`f<T>(nullptr)`)
- __builtin_source_location: a static record laid out as
  source_location::__impl, the function named as g++ does (`int main()`);
  as (or inside) a default argument, the caller's place
tests/cxx/cxx20misc5.cc (g++ agrees on both targets).

Bit-fields in a class with bases or virtual functions (written as a
packed C struct with explicit offsets): each run of them a run of C
bit-fields at the bit the Itanium layout chose, unnamed ones filling the
gaps — the bytes g++'s build has (tests/cxx/bitfields2.cc prints them).
For that, EmbCC's C reads and writes a packed struct's bit-field that
crosses its type's storage unit whole (byte by byte, as gcc does; it kept
only the unit's part), tests/exec/packed-bitfields.c. Also in EmbCC's C:
`__builtin_va_list` declares a variable in a function too
(std::to_string(double)'s).

**CX8's second half started: libstdc++'s sources compiled by EmbCC.**
tools/build-libstdcxx.sh compiles every object of the reference build of
libstdc++ (the configured tree tools/build-ref-gxx.sh leaves) from GCC's
sources, with its directory's standard and the build's include paths,
then puts them into copies of libstdc++.a and libsupc++.a (a stand-in
EMBCC_REF_GXX for the harness). libsupc++ — all 65 objects: exceptions and
the personality routine, RTTI and dynamic_cast, operator new/delete (the
aligned ones too), guards, the demangler, the fundamental types' typeinfo —
compiles on both targets, and tests/libstdcxx and tests/cxx behave with it
as with g++'s (tests/golden/cxx-libsupcxx.sh runs five of them). What it
took:
- `-std=` for C++: `__cplusplus` and the feature macros per standard, as
  g++ gives them (each EmbCC claims defined in the standards g++ defines
  it in, its value capped at what EmbCC does; tests/golden/cxx-std.sh);
  `__STRICT_ANSI__` for c++NN, `-fchar8_t`, `__GNUC_STDC_INLINE__` (newlib's
  inline functions are then `static inline`, not GNU89 `extern inline`,
  which EmbCC emitted as global definitions); `-D`/`-U` on the command line
- aligned allocation (C++17, `__cpp_aligned_new`): new and delete of an
  over-aligned type call the std::align_val_t operators — the class's own
  first, then sized and plain as they are viable; arrays, a new's cleanup
  when the constructor throws, deleting destructors
- alignas and `__attribute__((aligned))` on data members (they were
  ignored: a layout differing from g++'s), `__alignof__` of a member
- EmbCC's own include/unwind.h: the Itanium unwinder interface with GCC's
  extensions and libgcc's layouts (`_Unwind_Exception` 16-byte aligned)
- the fundamental types' typeinfo (and their pointers', const or not),
  emitted where `__cxxabiv1::__fundamental_type_info`'s key function is
  defined, as g++ does — the same symbols
- `__constinit`; `&f` of an overload set resolved by the parameter it
  converts to; `X::operator T()` outside X with T X's member; `?:` of a
  pointer to member and nullptr; `__builtin_eh_return_data_regno`,
  `__builtin_extend_pointer`
- EmbCC's C: flexible array members (and GNU's `[0]`), enumerator values
  that are constant expressions, a comma expression as an `if`/`while`/
  `for` condition, form feed and vertical tab as white space
  (tests/exec/c-extras2.c); tests/cxx/alignednew.cc.
**The src/ directories too.** tools/build-libstdcxx.sh now compiles
src/c++98 through src/c++26 with the Makefiles' per-file flags (the
`*_cow` objects with `-D_GLIBCXX_USE_CXX11_ABI=0`, format and print as
C++26, cxx11-ios_failure with its typeinfo rewritten as the Makefile
rewrites g++'s assembly — here in the lowered C), and puts each object in
place of the reference archive's member it matches by content (libtool
stored the second codecvt.o as lt1-codecvt.o). On x86-64, 191 of the 193
objects are EmbCC's (the two module units hold no code and are left out);
every program of tests/libstdcxx and tests/cxx linked with that library
prints and exits as g++'s build does. What remains: floating_from_chars
(fast_float multiplies in `__uint128_t`, and EmbCC has no `__int128` yet)
and tzdb (ranges::subrange's constructor is an abbreviated function
template, `C auto` parameter, which EmbCC does not read yet). On aarch64,
190 of 193 — floating_to_chars too, where newlib's fenv.h writes
`__asm __volatile("mrs ...")` — and the same 52 programs agree with g++.
What it took:
- explicit instantiation of an instance's static data members
  (`template T A<X>::m;`, and the `extern template` declaration), its
  destructor, a nested class's members, and GNU's `inline template class`
  (the vtable and RTTI, not the members); `template class` instantiates the
  members defined in the class too (they were left out), and never a
  member template's specializations that overload resolution made
- vague linkage as g++ gives it: a template instance's members defined
  outside the class, their local statics, and its vtable and RTTI are weak
  in every unit (they were strong: two units using one instance clashed);
  an explicit specialization is an ordinary function (strong, unless
  declared inline); a namespace-scope `constexpr` variable has internal
  linkage
- ABI tags (5.1.2): `__abi_tag__` on functions, variables, classes and
  inline namespaces, mangled `B <name>` after the name — and the implicit
  ones: a function whose return type carries tags its parameters and
  scopes do not (`std::string f()` is `_Z1fB5cxx11v`), a variable by its
  type. Without them EmbCC's names missed g++'s libstdc++'s
  (`std::locale::name[abi:cxx11]()`, `filesystem::current_path()`);
  tests/golden/cxx-abi.sh calls across both ways
- a member function's default arguments are read when first used, the
  class complete (`replace_extension(const path& = path())` in path);
  a function template's default arguments, and its declaration's default
  template arguments, survive its definition
- overload sets across inline namespaces are one set (`std::rotate` in
  std::_V2 beside pstl's in std); `struct N::X`, `N::f() { }` for what an
  inline namespace of N declares; a qualified class's bases looked up in
  its namespace
- pointers to inherited members (`&D::f` of B's f: a `B::*`, converting to
  D's); B* over void* in overload ranking; the default constructor
  inherited too (C++17) unless the class declares its own; `= default`
  after the class (user-provided: defined in that unit, not trivial); a
  trivial assignment from an object copies its bytes (it copied into a
  temporary it then destroyed)
- a typedef names an unnamed class for linkage (newlib's `mbstate_t`: the
  class had no linkage, so `codecvt<wchar_t, char, mbstate_t>` was local)
- constant locals read in a lambda without a capture (not odr-uses);
  GNU variable-length arrays (locals of scalars: C's VLA); `p->~X<T>()`;
  `(dependent::value)` as a template argument (a value, not a cast), and
  `<`, `<=>`, `->*`, `++`, assignment operators in dependent expressions;
  C++23 `if consteval` and the `z`/`uz` literal suffixes; a variable
  template initialized by an immediately-called lambda; `decltype(operator>
  (a, b))` in a partial specialization; `T[]` never matches `T[N]` (the
  extent_v partial specializations were ambiguous — ranges algorithms on
  built-in arrays failed)
- GNU asm statements in C++ (operands of C++ expressions, passed to C —
  random_device's cpuid and rdrand), EmbCC's own cpuid.h, and
  `__builtin_ia32_rdrand*_step`, `__builtin_ia32_rdseed*_step`,
  `__builtin_ia32_pause`, `__builtin_powi[fl]` (libgcc's `__powi?f2`)
- GNU's pointer-to-member-function conversions: `(void *)(obj.*pmf)` the
  function the call would reach, `(void *)&C::f` the function itself (the
  locale facets test for overridden do_get this way); a virtual function's
  pointer to member is now its vtable offset + 1, as Itanium says (it was
  the function, so a call through it was not virtual)
- attributes after a declarator's parameters (`void f(int)
  __attribute__((weak))`), and weak declarations: a weak undefined
  function or variable is a weak reference (0 if nothing defines it) —
  on aarch64 through the GOT (adrp/add cannot give 0), in EmbCC's C too
- the preprocessor: C++ raw string literals (no directives, splices or
  comments inside), `#elifdef`/`#elifndef` (C23/C++23, and GNU's before
  except strict C++20), `#line` and GNU's `# N "file"` linemarkers; asm
  labels of adjacent literals (newlib's `__ASMNAME`); `__cpp_sized_deallocation`;
  include/float.h complete (`LDBL_MANT_DIG` and the rest)
tests/cxx/libsources.cc (g++ agrees on both targets).
**Abbreviated function templates** (C++20, 9.3.4.6): a function with
`auto` or `C auto` parameters is a template of invented parameters, as a
generic lambda's operator() — at namespace scope, as members of classes
and of class templates, beside declared template parameters (appended
after them), packs (`auto...`); `C auto` gives the invented parameter C
as its type-constraint, so overloads are ordered by it. With them and a
few more fixes — `const T (&)[N]` bound to a `T[N]`, `T(*)[]` to `const
T(*)[]` (span's compatibility test), static_cast to a reference to an
unrelated class through a conversion function (string to const
string_view&), a constexpr definition of a member declared const — tzdb
compiles too: 192 of libstdc++'s 193 objects are EmbCC's on both targets,
all but floating_from_chars (`__uint128_t`), and the 54 programs agree
with g++ (tests/cxx/abbrev.cc, tests/libstdcxx/ranges2.cc). newlib's
aarch64 fenv.h needed `__asm`/`__volatile` spelled so, and EmbCC's aarch64
assembler FPCR and FPSR.

Pointers to member functions on aarch64 now take the ARM C++ ABI's form,
as g++ there does: ptr the function or the vtable offset, adj twice the
this adjustment plus 1 for a virtual one (x86-64 keeps Itanium's: ptr the
vtable offset + 1). tests/golden/cxx-abi.sh passes them between EmbCC's
code and g++'s and compares them, on both targets.

**`__int128`** (GNU), in C and in C++, on both targets: `__int128`,
`unsigned __int128`, `__int128_t`, `__uint128_t` — 16 bytes, 16-aligned,
ranked above long long in the usual arithmetic conversions. EmbCC's C
computes it in two eightbytes: addition and subtraction with the carry,
the bitwise operations, negation, comparisons and extensions inline;
multiplication, division, shifts and the float conversions through
libgcc's helpers (`__multi3`, `__divti3`, `__ashlti3`, `__floattidf`, ...),
as gcc calls them. It is passed as the ABIs say — x86-64 in two integer
registers (any two) or a 16-aligned stack slot, returned in rax:rdx;
aarch64 in an even register pair (AAPCS64 C.8, after an int in x0 it
takes x2:x3) or a 16-aligned stack slot, returned in x0:x1 — and va_arg
reads it so, and `__atomic_*` / `__sync_*` do it inline and lock-free
with a 16-byte compare-and-swap (x86-64's `lock cmpxchg16b`, aarch64's
exclusive pair; the rest are loops of it) where gcc calls libatomic. The
GENERIC atomic builtins — `__atomic_load`, `__atomic_store`,
`__atomic_exchange`, `__atomic_compare_exchange` — take any object of 1,
2, 4, 8 or 16 bytes rather than only an integer or pointer, because
their values travel by pointer and the operation is a byte copy; irgen
lowers such an object as the unsigned integer of the same size, which is
the identical instruction. That is what `std::atomic<double>` and
`std::atomic<SmallStruct>` need, and a tagged pointer under a 16-byte
compare-and-swap is why the second is wanted.
Bit-fields of it, packed ones across their unit too (17 bytes at most),
switch on it, static initializers folded in 128 bits (src/sema/w128.c).
A function using it is not optimized (IR passes, the inliner and x86-64's
register allocation skip it). C++ mangles it `n`/`o`, and evaluates it
in 128 bits: constant expressions (static_assert, template arguments, a
const static member no long holds) and constexpr functions of it, in the
interpreter (consteval.c: an integer there is 128 bits wide). libstdc++
compiled by EmbCC enables it: its integer traits, numeric_limits,
to_chars/from_chars, `<random>`'s 64-bit engines and distributions, and
Ryu's 128-bit arithmetic in floating_to_chars. So all 193 of libstdc++'s
objects are EmbCC's on both targets (floating_from_chars, whose
fast_float multiplies in `__uint128_t`, was the last).
tests/exec/int128.c, int128-more.c, int128-atomic.c,
packed-wide-bitfields.c; tests/golden/int128-abi.sh (an EmbCC half and a
gcc half calling each other: arguments past the registers, structs
holding one, va_arg of the other's arguments, a packed struct of its
bit-fields); tests/golden/cxx-abi.sh (overloads on it across compilers);
tests/cxx/int128.cc; tests/libstdcxx/int128.cc. `_Atomic` stays
volatile, as for every type. Found on the way: a packed 64-bit
bit-field at bit 1..7 of its first byte (9 bytes) lost its last byte;
`__builtin_memcpy`/`memmove`/`memset` needed a prototype in sight (gcc
knows theirs, and so does EmbCC now).

**Constant initialization** (6.9.3.2): a static object's integer
initializer that constant evaluation gives a value — a constexpr
function's call and all, `long x = f(3);` — is written as that value,
statically, as g++ does; it was computed at run time, so a dynamic
initializer running earlier (another object's constructor) read 0. Local
statics so initialized need no guard. And the unit's dynamic
initializers run in the order of their DEFINITIONS (6.9.3.3): an object
declared `extern` before was initialized where it was declared.
tests/cxx/constinit.cc.

**C++ on EmbLinkOS (CX8's acceptance).** The OS's own C++ program,
user/tests/cxxdemo/cxxdemo.cc, compiled by EmbCC with the flags the OS's
C++ rule uses and linked with the libstdc++ and libsupc++ EmbCC built
from GCC's sources — no g++ in the program or in the library — staged
onto an EmbLinkOS image (its STAGED_APPS, for binaries built outside its
tree, so nothing in the OS is modified) and run by the kernel's own
`test cxx`: global constructors and their order, new/delete and
new[]/delete[], templates, a function-local static (libsupc++'s guards),
std::string, std::vector, `<iostream>` (the library's ios_base::Init
constructor runs from the image's own .init_array), destructors at exit —
13 checks, exit 0. The OS had never run a C++ program before: its C++ is
gated on a g++ cross toolchain that is not on this machine.
tests/golden/x86_64/emblinkos-cxx.sh, opt-in (EMBCC_OS_CXX=1: it rebuilds
the image and boots QEMU). EmbCC also takes the flags that build line
passes: `-W...` (it has one warning level, and its diagnostics are
errors), `-fno-stack-protector` (what it does; `-fstack-protector` is
refused rather than ignored), `-fno-rtti`/`-frtti` (accepted; EmbCC
always emits RTTI, so typeid and dynamic_cast keep working).

**C++ compiled ON EmbLinkOS (CX9).** The toolchain that does it is
EmbCC's own and runs on the metal: embcc.elf — EmbCC's output, EmbLD-linked
(docs/developer/selfhost-on-os.md) — compiles a C++ program the kernel writes to
/data/tmp, embld.elf links it against the sealed ABI (/system/abi:
crt0.o, syscalls.o, libc.a), and the kernel runs it and reads its exit
code: the OS's own `test embcc cxx` oracle. No host compiler, no tcc, no
gcc in that loop. The program is self-contained C++ — it defines
operator new/delete, a local static's guard and __cxa_pure_virtual — so
nothing but the C ABI need be on the image: what is judged is the
compiler, not a C++ library. It uses a global constructor, virtual
dispatch through a base, templates over two types, new/delete and
new[]/delete[], a guarded function-local static and recursion, and exits
42. tests/golden/x86_64/emblinkos-cxx-onos.sh, opt-in (EMBCC_OS_CXX=1).

That needs `-fno-rtti` to mean it, and it now does, as g++ does: no
typeinfo object is written, a vtable's typeinfo slot is null, and typeid
and dynamic_cast are refused — which is what lets freestanding C++ link
against the C ABI alone, with no libsupc++ for __class_type_info's
vtables. tests/golden/cxx-no-rtti.sh (g++ agrees, both targets).

**The suites wholly on EmbCC's library.** `make test-libstdcxx`
(tests/golden/cxx-libstdcxx-embcc.sh, opt-in: the library takes minutes
to build) builds libstdc++ and libsupc++ from GCC's sources with EmbCC —
every object, or it fails — and links every program of tests/libstdcxx
and tests/cxx with that library: each must exit and print as g++'s build
does with g++'s library, on both targets (57 programs each).

**`consteval`: immediate functions** (7.7). Every call of a consteval
function outside another's body (and outside `if consteval`) is evaluated
when the program is compiled — before the function holding it is written
(emit.c) — and must be a constant expression. When it is not, it is an
error that says why: the constant evaluator (consteval.c) now tells a
failure that makes an expression not a constant one (undefined
behaviour, a throw, a call of a function that is not constexpr — with
the string it was passed, a throw helper's message —, a write to a
constant, a read of what is not one) from one only because it does not
model something; the second runs at run time, the function being
constexpr too. An integer's value is written as a constant; a class's is
built at run time by the same code, once its evaluation succeeded. For
the evaluator to follow libstdc++'s format checking it now does virtual
calls — each object's dynamic type recorded as its constructor, bases and
members built, runs; the final overrider the first class declaring one on
the path down to the called function's — and bit-fields (read, written,
initialized, by their bits). So std::format's strings are checked when
the program is compiled, as with g++: `std::format("{:d}", "x")` is an
error (tests/golden/cxx-format-check.sh: EmbCC and g++ agree on which of
12 strings are ill-formed, both targets). tests/cxx/consteval.cc;
tests/golden/cxx-reject.sh (runtime arguments, division by zero, throw,
a non-constexpr call, a non-constant global).

Not yet (CX6): a generic lambda's conversion to a pointer to function;
constexpr objects of class type are still initialized at run time (their
values are known to the interpreter, not yet written as static data);
`new`/`delete`, virtual bases, long double and unions in constant
evaluation; `constinit` is accepted but not checked; a structured binding
at namespace scope or of a class whose members are in a base.
