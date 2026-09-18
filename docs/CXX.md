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
| **CX3** | inheritance (single, multiple, virtual), virtual functions and vtables, pure virtuals, RTTI (`typeid`, `dynamic_cast`) | g++ calling EmbCC virtuals and back |
| **CX4** | templates: class, function, alias and variable templates; deduction; explicit and partial specialization; SFINAE; variadics and fold expressions | |
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
  resolution; `class.c` lays classes out and builds objects; `mangle.c`
  names things as the Itanium ABI does; `emit.c` writes C.
- A reference is a pointer in C; a member function takes `this` first; a
  constructor or destructor is two functions, C2/D2 (the base-object body)
  and C1/D1 (the complete object, calling it).
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

**CX3 done, but for virtual bases** (September 2026): single and multiple
inheritance with g++'s layouts, virtual functions (overriding with and
without `virtual`, `override`/`final`, pure virtuals and abstract
classes, virtual destructors and deleting destructors, thunks), `typeid`
and `dynamic_cast` (down, across, to `void *`, failing, of references).
tests/cxx `inherit` and `rtti` agree with g++ on both targets, and
cxx-abi splits one hierarchy across the two compilers — each calling the
other's virtual functions, g++'s dynamic_cast reading EmbCC's typeinfo,
deleting through a thunk to EmbCC's D0, layouts compared number for
number.

Virtual base classes are the one CX3 piece left, as **CX3b**: vbase
offsets, vcall offsets and virtual thunks, VTTs and construction vtables
(`_ZTT`, `_ZTC`), the base-object constructors that take a VTT — g++'s
exact output for a diamond is recorded and is the target. libstdc++'s
stream classes use virtual bases, but through explicit instantiations
compiled into libstdc++ itself; templates (CX4) come first.

Already there ahead of their milestones: `enum class` and fixed underlying
types, `auto` and `decltype` for variables, `if`/`switch` with an
initializer, `static_assert`, delegating constructors, default member
initializers.

Refused until later, each naming its milestone: virtual base classes
(CX3b); templates (CX4); exceptions (CX5); lambdas, range-`for`,
`initializer_list`, deduced return types (CX6); designated initializers,
`<=>` and C++20's rewritten comparisons, coroutines (CX7). Access control
is parsed but not yet enforced; anonymous struct/union members, bit-fields
in a class with bases or virtual functions, and copying arrays of
non-trivially copyable objects are not supported yet.

Next: CX4 (templates), then CX3b.
