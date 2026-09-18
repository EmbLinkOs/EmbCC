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

## Status

**CX1 done** (September 2026): tests/cxx — `basics`, `classes`, `scopes`,
`globals` — run on both targets and agree with g++, and
`tests/golden/cxx-abi.sh` links an EmbCC half with a g++ half that call
each other (all the builtin types, qualifiers, substitutions, function
pointers, arrays, enums, a nested class, a class by value, `std::`, and
g++ constructing EmbCC's class). The OS's cxxdemo checks 1–3, 5 and 6 pass
on both targets (4 needs templates, CX4).

Already there ahead of their milestones: `enum class` and fixed underlying
types, `auto` and `decltype` for variables, `if`/`switch` with an
initializer, `static_assert`, delegating constructors, default member
initializers.

Refused until later, each naming its milestone: operator overloading,
conversion functions, non-trivial copies and classes with them passed or
returned by value (CX2); base classes, virtual functions, `typeid`,
`dynamic_cast` (CX3); templates (CX4); exceptions (CX5); lambdas,
range-`for`, `initializer_list`, deduced return types (CX6); designated
initializers, `<=>`, coroutines (CX7). Access control is parsed but not yet
enforced; anonymous struct/union members are not supported yet.

Next: CX2.
