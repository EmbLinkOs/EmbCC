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
| **CX8** | libstdc++: its headers, then its sources, compiled by EmbCC | the OS's cxxdemo, with `<iostream>` |
| **CX9** | C++ on EmbLinkOS: embcc compiling C++ on the metal, over newlib and emlibc | on-OS tests |

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
`initializer_list`, deduced return types (CX6); designated initializers, `<=>` and C++20's
rewritten comparisons, `auto` parameters, coroutines (CX7). Access control
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

Next: CX6.
