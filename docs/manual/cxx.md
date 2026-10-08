# C++ support

This page describes the C++ that EmbCC compiles: how a C++ translation
unit is selected, which language level is parsed, the status of each
language feature from C++98 to C++23, the C++ ABI, and the run-time
support a C++ program needs, including EmbCC's own C++ library and GCC's
libstdc++. It is for people who compile C++ with EmbCC. The options are
also listed in [Invoking EmbCC](invoking.md).

## Summary

EmbCC parses C++20 with GNU extensions. Most of the core language of
C++98 through C++20 is implemented, and several C++23 features are too.
The largest gaps are modules, two-phase name lookup, and parts of
constant evaluation. The [feature tables](#language-features) give the
status of each feature and the exact diagnostic for each refusal.

C++ is supported on the x86-64 and AArch64 ELF targets. There, objects
follow the Itanium C++ ABI as g++ does: they link with objects compiled
by g++ and with libstdc++, and exceptions propagate between the two. The
headers and the sources of GCC's libstdc++ compile with EmbCC. EmbCC
also ships its own C++ runtime and standard library (`lib/libcxx`).

On the Darwin and Windows targets C++ works with restrictions. On the
32-bit ARM targets (Cortex-M and ARM state) and on `riscv32-unknown-elf`
C++ is supported without exceptions (`-fno-exceptions`), with or without
RTTI, following the ARM C++ ABI and the Itanium ABI's 32-bit form;
objects link with clang++'s. On `riscv64-unknown-elf` C++ is not
supported: a unit compiles when exceptions are turned off, and is not
tested. On AVR, MIPS32, Xtensa and TriCore EmbCC refuses to generate
code for C++. See [Targets](#targets).

EmbCC compiles C++ by lowering it to C, which the C front end, the
optimizer and the code generators then compile (design decision D-013 in
[Decisions](../internals/decisions.md)). The consequences a user can see
are listed in [How C++ is compiled](#how-c-is-compiled).

## Selecting C++

### File names

A file is compiled as C++ when its name ends in one of these suffixes:

| Suffix | |
|---|---|
| `.cc`, `.cpp`, `.cxx`, `.C`, `.c++`, `.cp`, `.CPP`, `.ii` | C++ source |

The suffix test is case-sensitive: `.C` and `.CPP` are C++, `.c` is C. A
`.ii` file is preprocessed again like any other C++ source.

### `-x c++`

`-x c++` (or `-xc++`) compiles the input as C++ whatever its suffix.
`-x c++-cpp-output` is accepted as a synonym. `-x c` and `-x cpp-output`
select C, and `-x none` returns to choosing by suffix. Any other language
name is refused:

```text
embcc: error: unknown language 'LANGUAGE' for -x (c or c++)
```

### Language level: `-std=STANDARD`

Without `-std=`, a C++ unit is compiled as `-std=gnu++20`. EmbCC always
parses C++20 (with the C++23 additions listed below); `-std=` selects the
value of `__cplusplus`, the feature-test macros, and whether
`__STRICT_ANSI__` is defined. It does not make EmbCC reject a newer
construct.

| `-std=` | `__cplusplus` |
|---|---|
| `c++98`, `c++03`, `gnu++98`, `gnu++03` | `199711L` |
| `c++11`, `c++0x`, `gnu++11`, `gnu++0x` | `201103L` |
| `c++14`, `c++1y`, `gnu++14`, `gnu++1y` | `201402L` |
| `c++17`, `c++1z`, `gnu++17`, `gnu++1z` | `201703L` |
| `c++20`, `c++2a`, `gnu++20`, `gnu++2a` (default) | `202002L` |
| `c++23`, `c++2b`, `gnu++23`, `gnu++2b` | `202302L` |
| `c++26`, `c++2c`, `gnu++26`, `gnu++2c` | `202400L` |

- The `c++NN` spellings define `__STRICT_ANSI__`. They also leave
  `__GLIBCXX_TYPE_INT_N_0` and `__GLIBCXX_BITSIZE_INT_N_0` undefined, so
  libstdc++ does not treat `__int128` as an integer type.
- `-std=c++20` and the earlier strict spellings do not recognize
  `#elifdef` and `#elifndef`. Every other mode does.
- A standard earlier than C++20 draws a warning, because it is not
  enforced:

  ```text
  embcc: warning: -std=c++17 sets the standard macros but is not enforced; EmbCC parses C++20 and will accept newer constructs
  ```

- `-std=c++23` and `-std=c++26` change only `__cplusplus`; the
  feature-test macros are those of C++20.
- An unknown C++ standard is an error:
  `embcc: error: unknown C++ standard '-std=c++29'`.
- A C standard name (`-std=c11`, ...) given for a C++ unit is handled as
  for C and leaves the C++ mode at its default.

### C++ options

#### `-fno-exceptions`, `-fexceptions`

Exceptions are on by default. `-fno-exceptions` turns them off:
`__cpp_exceptions` and `__EXCEPTIONS` are not defined, no landing pads
are generated, and `throw` and `try` are errors:

```text
error: 'throw' with exceptions disabled (-fno-exceptions)
error: 'try' with exceptions disabled (-fno-exceptions)
```

`-fexceptions` turns exceptions back on and also requests unwind tables.

#### `-fno-rtti`, `-frtti`

RTTI is on by default. Under `-fno-rtti` no `type_info` objects are
written, the type-information slot of each vtable is null, and `typeid`
and `dynamic_cast` are errors (an upcast with `dynamic_cast` included):

```text
error: typeid with -fno-rtti
error: dynamic_cast with -fno-rtti
```

`__cpp_rtti` and `__GXX_RTTI` remain defined under `-fno-rtti`.

#### `-fno-access-control`, `-faccess-control`

`-fno-access-control` stops enforcing `private` and `protected`, as the
GCC option does. `-faccess-control` restores the default. See
[Access control](#access-control).

#### `-fchar8_t`

Defines `__cpp_char8_t` in modes before C++20. `char8_t` is a keyword in
every mode. `-fno-char8_t` is not accepted.

#### `-funwind-tables`, `-fno-unwind-tables`

A C++ unit gets unwind tables (`.eh_frame`) unless both
`-fno-exceptions` and `-fno-unwind-tables` are given.

#### `--emit-c`

Writes the C that the C++ unit is lowered to, to standard output or to
the `-o` file, instead of compiling it. Given a C file, the driver stops
with `embcc: error: --emit-c lowers C++; 'FILE' is C`.

#### g++ options that are not accepted

`-fno-threadsafe-statics`, `-fpermissive`, `-fno-char8_t`,
`-fsized-deallocation`, `-fno-sized-deallocation`,
`-fno-elide-constructors`, `-fcoroutines`, `-fconcepts`,
`-fvisibility=...` and `-nostdinc++` are refused:

```text
embcc: error: unknown argument '-fno-threadsafe-statics'
```

## How C++ is compiled

The C++ front end parses the unit, performs semantic analysis, and writes
C: classes become structures, member functions become functions with an
explicit `this` parameter under their mangled names, references become
pointers, and constructor, destructor and cleanup calls become explicit.
That C is compiled by the same pipeline as a C source. What follows from
this:

- **Errors.** The C++ front end reports every error it finds, then the
  compile stops before the C stage. An error inside a template carries
  `note: in the instantiation of ...` lines.
- **Warnings.** The C++ front end itself issues one warning
  (`section("NAME") on function 'F' is ignored`). The other warnings come
  from the C stage; five of them are switched off for C++. See
  [Warnings in C++](diagnostics.md#warnings-in-c).
- **Optimization.** `-O` levels apply to the lowered C. A function that
  has landing pads for calls that can throw is not inlined, and some
  optimizations, and register allocation on AArch64, are not applied to
  it. Calls of `noexcept` functions and destructors need no landing pad.
- **Debug information.** `-g` describes the lowered program. The
  compilation unit's language is C99, functions are named by their
  mangled names, and a class appears as a structure named `_C` followed
  by the length of its name and the name (`_C5Shape`). Line numbers refer
  to the C++ source. See [Debugging](debugging.md).
- **Data model.** The C++ front end computes `sizeof`, `alignof`, class
  layout and constant expressions itself, by the target's data model, as
  the C front end does: `long` and pointers are 8 bytes on the 64-bit
  targets and 4 on 32-bit ARM and RV32, where `size_t` is `unsigned int`
  and `ptrdiff_t` is `int` (and are mangled `j` and `i`). `long double`
  has the target's size and alignment: 16 bytes on x86-64, on AArch64
  ELF and Linux, and on RISC-V, and 8 on `arm64-apple-darwin` and 32-bit
  ARM. A target whose C++ ABI is not implemented refuses C++ code
  generation; see [Targets](#targets).

## Targets

| Target | C++ | Exceptions | Run-time library |
|---|---|---|---|
| `x86_64-elf`, `aarch64-elf`, `-emblink` | Supported | Supported | `libcxx.a`, or libsupc++/libstdc++; libgcc's unwinder |
| `x86_64-linux-gnu`, `aarch64-linux-gnu` | Supported | Supported | `libcxx.a`; the unwinder in EmbCC's `librt.a` |
| `arm64-apple-darwin` | Supported | Supported | the system's C++ runtime |
| `x86_64-apple-darwin` | Supported | Objects do not link | the system's C++ runtime |
| `x86_64-windows-gnu` | Restricted | Not supported | none |
| `riscv64-unknown-elf` | Not supported; compiles, untested | Not supported | none |
| 32-bit ARM: Cortex-M (`thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabi[hf]`, `thumbv8m.main-none-eabi[hf]`) and `armv7a-none-eabi[hf]` | Supported with `-fno-exceptions` | Refused | the embedded `libcxx.a` (`make libcxx-embedded`) |
| `riscv32-unknown-elf` | Supported with `-fno-exceptions` | Refused | the embedded `libcxx.a` (`make libcxx-embedded`) |
| `avr`, `mipsel-none-elf`, `mips-none-elf`, `xtensa-none-elf`, `tricore-none-elf` | Refused | Not supported | none |

**x86-64 and AArch64 ELF.** These are the C++ targets. `libcxx.a` is
built for `x86_64-elf`, `aarch64-elf`, `x86_64-linux-gnu` and
`aarch64-linux-gnu`. When `embcc` links a C++ program itself (x86-64 ELF
targets only), it adds `libcxx.a` before `libc.a`; when the target's
`libcxx.a` is missing it stops with:

```text
embcc: error: no libcxx.a for TRIPLE -- a C++ program needs the C++ runtime, and this target's is not built or not installed
```

**Darwin.** Objects follow the platform's C++ ABI and link with the
system's C++ runtime through the system linker; on `arm64-apple-darwin`,
exceptions thrown by EmbCC code are caught by Clang-compiled code and the
reverse. `long double` has the platform's size, 8 bytes on
`arm64-apple-darwin` and 16 on `x86_64-apple-darwin`, so a class that
holds one is laid out as Clang lays it out. `thread_local` is refused,
as `__thread` is on Darwin. On `x86_64-apple-darwin`, an object
that calls the exception runtime (any unit with landing pads or a
`throw`) does not link with Apple's linker:

```text
ld: fixup error (kind=x86_64_rip) at '__ZN1BD2Ev'+0x99 from nox.o, target '___cxa_call_terminate' does not have address
```

Compile for `x86_64-apple-darwin` with `-fno-exceptions`.

**Windows.** A C++ unit compiles only with both `-fno-exceptions` and
`-fno-unwind-tables`. Without them every C++ unit is refused:

```text
error: C++ exceptions are not supported for a Windows target yet: the unwind tables go in .pdata and .xdata and neither is written
```

The dynamic initialization of namespace-scope objects is not registered
in the COFF object, so it does not run. See [Windows](targets.md#windows-coff)
for the other limits of that target.

**32-bit ARM and RV32.** C++ is compiled for the Cortex-M targets, ARM
state (`armv7a-none-eabi`) and `riscv32-unknown-elf` without exceptions:
the subset firmware and RTOS wrappers are written in --
classes, constructors and destructors, virtual functions and abstract
classes, multiple and virtual inheritance, templates, namespaces,
references, operator overloading, `constexpr`, static objects with
constructors, function-local statics, placement `new`, `new[]` and
`delete[]`, pointers to members and lambdas, and with RTTI `typeid` and
`dynamic_cast`. The objects follow the
Itanium C++ ABI's 32-bit form, and on ARM the ARM C++ ABI's changes to
it:

| | ARM (EABI) | RV32 |
|---|---|---|
| `size_t`, `ptrdiff_t` | `unsigned int`, `int` (`_Znwj`) | `unsigned int`, `int` (`_Znwj`) |
| vtable entries, offsets | 4 bytes | 4 bytes |
| constructors, complete and base-object destructors | return `this` | return nothing |
| pointer to member function | `{ ptr, adj }`: a virtual one's `ptr` is the vtable offset and `adj` is twice the adjustment plus 1 | `{ ptr, adj }`: a virtual one's `ptr` is the vtable offset plus 1 |
| guard variable | 32 bits; initialized when bit 0 is set | 64 bits; initialized when the first byte is non-zero |
| array cookie | 8 bytes at the start of the allocation: the element size, then the count | the count, in the 4 bytes before the elements |
| static destructors registered with | `__aeabi_atexit` | `__cxa_atexit` |
| `__STDCPP_DEFAULT_NEW_ALIGNMENT__` | 8 | 16 |
| `va_list` mangled as | `St9__va_list` | `Pv` |

Each of these is checked against clang++: `tests/golden/cxx-abi-ilp32.sh`
links EmbCC and clang++ objects calling each other both ways on a
Cortex-M3, a Cortex-M4F and RV32, and compares what the two compilers
say about sizes, offsets, cookies and the data the ABI lays out, and with
RTTI casts across classes whose `type_info` the other compiler wrote. clang++
itself registers static destructors with `__cxa_atexit` on ARM; the
runtime provides both.

The run-time support is the embedded `libcxx.a`, built by `make
libcxx-embedded` (`tools/build-libcxx.sh TRIPLE OUTDIR`) into
`build/libcxx/TRIPLE/`: `operator new` and `operator delete` over
`malloc` (weak, so a program may replace any of them), the guard
functions `__cxa_guard_acquire`, `__cxa_guard_release` and
`__cxa_guard_abort`, `__cxa_pure_virtual`, `__aeabi_atexit` and
`__dso_handle`; for RTTI `std::type_info`, the `__cxxabiv1` type-information
classes and `__dynamic_cast`; and `__cxa_bad_cast` and `__cxa_bad_typeid`,
which stop the program (`__builtin_trap`), there being no exception to
throw: a failed `dynamic_cast` to a reference, or `typeid` of `*p` with
`p` null. `__cxa_atexit` is in the target's `libc.a`. Link it
before `libc.a` and `librt.a`. The startup code must run the
constructors in `.init_array` (between `__init_array_start` and
`__init_array_end`) before `main`, as the test harnesses' startups do.
Static destructors run only if the program calls `exit`.

Exceptions are refused, because EmbCC writes no ARM EHABI unwind tables
(`.ARM.exidx`) and no RISC-V `.eh_frame`. Exceptions are on by default,
so a C++ unit compiled without `-fno-exceptions` stops with:

```text
embcc: error: C++ exceptions are not supported for thumbv7m-none-eabi yet: EmbCC writes no ARM EHABI unwind tables (.ARM.exidx); compile with -fno-exceptions
```

An explicit `-funwind-tables` or `-fasynchronous-unwind-tables` is
refused the same way (`unwind tables are not supported for TRIPLE yet
... EmbCC writes no ARM unwind tables (.ARM.exidx)`), and without it a
C++ unit writes no `.eh_frame`. RTTI is on by default; `-fno-rtti`
leaves out the type-information objects and the code that reads them.

**AVR, MIPS32, Xtensa and TriCore.** The C++ ABI of these targets is not
implemented, and EmbCC refuses to generate code for a C++ unit there,
whether with `-c`, `-S` or `--emit-c`:

```text
embcc: error: C++ is not yet supported for avr: the C++ front end follows the C++ ABI of x86-64, AArch64, 32-bit ARM and riscv32, and this target's (2-byte pointers) is not implemented
```

`-fsyntax-only`, which writes nothing, is accepted.

**`riscv64-unknown-elf`.** The front end's data model is the target's,
and a C++ unit compiles, but C++ is not supported there:

- No `libcxx.a` is built for the target.
- Exceptions are not implemented. With exceptions on, any function that
  needs a landing pad is refused, and that includes a function whose
  local object has a destructor:

  ```text
  error: the RV64 backend cannot lower this operation yet (function F) [landing w=8 size=4]
  ```

- The `.eh_frame` section a C++ unit gets cannot be used by an unwinder;
  `-fno-unwind-tables` removes it.

A unit compiled with `-fno-exceptions -fno-unwind-tables` (and
`-fno-rtti`, there being no runtime to supply `type_info`) produces an
object, but EmbCC does not test C++ on this target.

## Language features

Status values: **Supported** means the feature compiles and behaves as
the standard says, within the general limits on this page. **Partial**
means it works with the stated exceptions. **Not supported** means EmbCC
refuses it, with the diagnostic shown. Diagnostics are quoted without the
`embcc: FILE:LINE:COL:` prefix.

### C++98 and C++03

| Feature | Status | Notes |
|---|---|---|
| Classes: data members, member functions, `this`, `static` members, `mutable`, `const` member functions | Supported | |
| Constructors, destructors, member initializer lists, implicit special members | Supported | Destructors run on every way out of a scope: its end, `return`, `break`, `continue`, `goto`. |
| Access specifiers and `friend` | Partial | Enforced, with gaps; see [Access control](#access-control). |
| Single and multiple inheritance | Supported | Layout as the Itanium ABI specifies. |
| Virtual inheritance | Supported | Virtual bases, VTTs and construction vtables as g++ emits them. |
| Virtual functions, pure virtual functions, abstract classes | Supported | An object of an abstract class: `an object of abstract class 'A'`. |
| Function overloading, default arguments | Supported | |
| Operator overloading | Supported | Including `->`, `()`, `[]`, both forms of `++` and `--`, conversion functions. |
| Argument-dependent lookup | Supported | |
| References | Supported | |
| `new`, `delete`, array forms, placement `new`, class-specific `operator new` and `operator delete` | Supported | |
| Namespaces, using-declarations, using-directives, unnamed namespaces, namespace aliases | Supported | |
| `extern "C"` and `extern "C++"` | Supported | |
| Class, function and member templates | Supported | |
| Explicit and partial specialization | Supported | The most specialized partial specialization is chosen. |
| Template template parameters | Supported | |
| Explicit instantiation | Supported | Not for variable templates (C++14 row). |
| Two-phase name lookup | Not supported | Names in a template are looked up when it is instantiated; see [Name lookup in templates](#name-lookup-in-templates). |
| `export` templates | Not supported | `modules are not supported` |
| `throw`, `try`, `catch`, function-try-blocks, `throw()` | Supported | On the targets in [Targets](#targets). |
| `typeid`, `dynamic_cast`, `std::type_info` | Supported | Errors under `-fno-rtti`. |
| `static_cast`, `const_cast`, `reinterpret_cast` | Supported | |
| Pointers to members | Supported | |
| `bool`, `wchar_t` | Supported | |
| Local classes; anonymous unions and structures in classes | Supported | |
| Bit-fields, also in classes with bases or virtual functions | Supported | |
| Function-local `static` objects | Supported | Initialization is guarded by `__cxa_guard_acquire` and `__cxa_guard_release`. |
| Namespace-scope objects with constructors and destructors | Partial | Constructors run from `.init_array`, in definition order within a unit; destructors are registered with `__cxa_atexit`. An array of objects with destructors at namespace scope: `a namespace-scope array of objects with destructors is not supported yet` |
| Copying or assigning a class whose member is an array of a class with a non-trivial copy or assignment | Not supported | `copying an array of 'S' is not supported yet`; `assigning an array of objects is not supported yet` |

### C++11

| Feature | Status | Notes |
|---|---|---|
| `auto`, `decltype`, trailing return types | Supported | |
| Rvalue references, move construction and assignment, implicit move on `return` | Supported | Named return value optimization where g++ applies it. |
| Lambdas | Supported | Captures by copy, by reference, `this`, capture defaults; conversion of a captureless lambda to a function pointer. |
| Range-based `for` | Supported | Over arrays, `begin`/`end` members or functions found by ADL, and braced lists. |
| `constexpr` | Supported | See [Constant evaluation](#constant-evaluation). |
| `std::initializer_list`, list-initialization | Partial | Needs `<initializer_list>`; without it: `std::initializer_list is not declared (#include <initializer_list>)`. An initializer on array `new`: `an initializer for new[] is not supported yet`. Narrowing conversions are not diagnosed. |
| `enum class`, fixed underlying types, opaque enum declarations | Supported | |
| `static_assert` | Supported | `static assertion failed: MESSAGE` |
| `nullptr`, `std::nullptr_t` | Supported | |
| `= default`, `= delete` | Supported | `use of deleted function 'f'` |
| `override`, `final` | Partial | Accepted and not checked; see [Checks EmbCC does not make](#checks-embcc-does-not-make). |
| Delegating and inheriting constructors | Supported | |
| Default member initializers | Supported | |
| Variadic templates, `sizeof...` | Supported | |
| Alias templates | Supported | |
| `noexcept` specifier and operator | Supported | A `noexcept` function that throws calls `std::terminate`. |
| Explicit conversion functions | Supported | |
| User-defined literals | Supported | Integer, floating, raw, character-pack template and string forms. |
| Attributes `[[...]]`, `alignas`, `alignof` | Supported | |
| `thread_local` | Supported | Lowered to `__thread`; available where `__thread` is ([Targets](targets.md)). |
| `char16_t`, `char32_t`; `u`, `U`, `u8` and raw string literals | Supported | A `u8` string literal has type `const char8_t[N]` in every mode. |
| Unrestricted unions | Supported | |
| `extern template` | Supported | |
| Expression SFINAE | Supported | |
| Local and unnamed types as template arguments | Supported | |
| `long long` | Supported | |
| Inline namespaces | Supported | |
| Ref-qualified member functions | Supported | |
| `>>` closing two template argument lists | Supported | |

### C++14

| Feature | Status | Notes |
|---|---|---|
| Generic lambdas | Partial | A generic lambda does not convert to a function pointer: `no conversion from '<anonymous>' to 'int(int)*' in initialization`. |
| Init-captures | Supported | |
| Return type deduction, `decltype(auto)` | Supported | |
| Relaxed `constexpr` | Supported | Loops, local variables and assignment in constant evaluation. |
| Variable templates | Partial | Explicit and partial specialization work. Explicit instantiation: `explicit instantiation of a variable template is not supported yet` |
| Binary literals, digit separators | Supported | |
| `[[deprecated]]` | Partial | Accepted; no warning is issued. |
| Sized deallocation | Supported | Deleting destructors call `operator delete(void *, std::size_t)`. |
| Aggregates with default member initializers | Supported | |

### C++17

| Feature | Status | Notes |
|---|---|---|
| Structured bindings | Partial | Arrays, data members, tuple-like classes; in declarations and range-`for`; `static`; captured by lambdas. Members of a base class: `binding the members of a base of 'D' is not supported yet`. At namespace scope: `a structured binding at namespace scope is not supported yet` |
| `if constexpr` | Supported | The discarded branch is not instantiated. |
| `if` with an initializer | Supported | |
| `switch` with an initializer | Supported | `switch (init; cond)`, with a declaration in the condition too. |
| Fold expressions | Supported | All four forms. |
| Inline variables | Supported | |
| Nested namespace definitions (`namespace a::b`) | Supported | |
| Class template argument deduction, deduction guides | Supported | |
| `template <auto>` | Supported | |
| `constexpr` lambdas, `[*this]` | Supported | |
| Guaranteed copy elision | Supported | |
| `noexcept` in the function type | Supported | |
| Over-aligned `new` and `delete` | Supported | `std::align_val_t` overloads. |
| Hexadecimal floating literals | Supported | |
| `__has_include`, `__has_cpp_attribute` | Supported | |
| `[[fallthrough]]`, `[[maybe_unused]]`, `[[nodiscard]]` | Supported | `[[nodiscard]]` is accepted; discarding the value is not diagnosed. |
| Aggregates with base classes | Supported | |
| `auto x{1}` deduces `int` | Supported | |
| `u8` character literals | Supported | Of type `char8_t` in every mode, as a `u8` string is; one UTF-8 code unit, so `u8'\u00e9'` is refused by name. |
| Removal of dynamic exception specifications and `register` | Not enforced | `throw(T)` and `register` are accepted. |

### C++20

| Feature | Status | Notes |
|---|---|---|
| Concepts, requires-clauses, requires-expressions | Supported | Constraints are normalized; subsumption orders overloads and partial specializations. |
| Abbreviated function templates (`auto` and `C auto` parameters) | Partial | Not in the parameters of a function type, such as `void (*fp)(auto)`: `an 'auto' parameter (an abbreviated template, C++20) is not supported yet (CX7)` |
| `<=>`; defaulted `==` and `<=>`; rewritten comparison candidates | Partial | `<compare>` must be included. A defaulted comparison in a class with an array member: `a defaulted 'operator==' of a class with an array member is not supported yet` |
| `consteval` | Supported | A call that is not a constant expression is an error naming the reason, for example `call to consteval function 'sq' is not a constant expression: the value of 'y' is not a constant` |
| `constinit` | Partial | Accepted and not checked. |
| Designated initializers | Supported | Member designators in declaration order. |
| Coroutines: `co_await`, `co_yield`, `co_return` | Supported | Symmetric transfer, `await_transform`, promise allocation functions. `co_await` in the initializer of a `static` local: `co_await here is not supported yet` |
| Modules | Not supported | `export`: `modules are not supported`. `module` and `import`: `expected a declaration before 'module'`, `expected a declaration before 'import'` |
| `using enum` | Supported | |
| Template parameter lists on lambdas; lambdas in unevaluated operands; default-constructible captureless lambdas; `[=, this]` | Supported | |
| Pack expansion in an init-capture (`[...xs = a]`) | Not supported | `expected a capture` |
| `char8_t` | Supported | |
| `[[likely]]`, `[[unlikely]]`, `[[no_unique_address]]` | Supported | `[[no_unique_address]]` lays members out as g++ does. |
| `explicit(bool)` | Supported | |
| Class types and floating-point types as non-type template parameters | Not supported | For example `no conversion from 'Pt' to 'bool' in a template argument`, or `template argument 1 of 'C' is not an integral constant` |
| `constexpr` virtual functions; `try` in `constexpr` functions | Supported | |
| `constexpr` dynamic allocation; changing a union's active member in a constant expression | Not supported | The expression is not a constant expression. |
| Parenthesized aggregate initialization | Supported | |
| Range-based `for` with an initializer | Supported | The initializer runs once, before the range is evaluated. |
| ADL for a function template called with explicit template arguments | Not supported | `'f' was not declared in this scope` |
| Class template argument deduction for aggregates | Supported | |
| Class template argument deduction for alias templates | Not supported | `expected a declaration before 'W'` |
| Default member initializers for bit-fields | Supported | |
| `namespace a::inline b` | Supported | Reopening a namespace as inline that was first declared otherwise is refused, as the standard requires. |
| `typename` optional in more contexts | Supported | |
| `__VA_OPT__` | Supported | |
| `std::source_location` | Supported | Through `__builtin_source_location`; a default argument gives the caller's position. |
| `std::is_constant_evaluated` | Supported | Through `__builtin_is_constant_evaluated`. |
| `std::bit_cast` | Partial | `__builtin_bit_cast` works at run time and is not a constant expression. |

### C++23

| Feature | Status | Notes |
|---|---|---|
| Explicit object parameters (`this Self &&self`) | Partial | In member functions. A lambda cannot call itself through its explicit object parameter: `calling member function 'operator()' without an object` |
| `if consteval` | Supported | |
| `static operator()` | Supported | |
| `z` and `uz` literal suffixes | Supported | |
| `#elifdef`, `#elifndef` | Supported | |
| `[[assume]]` | Accepted | |
| Multidimensional subscript operator | Not supported | `'M' has no viable operator[]` |
| `auto(x)`, `auto{x}` | Supported | A prvalue copy of `x`, of its decayed type. |

## Notes on partial support

### Name lookup in templates

A template's tokens are kept, and each instantiation parses them again
with the template arguments bound. Two-phase lookup is therefore not
implemented:

- A name that does not depend on a template parameter is looked up at
  the point of instantiation, not at the template's definition. In this
  program g++ calls `g(double)`; EmbCC calls `g(int)`:

  ```cpp
  void g(double);
  template <class T> int f() { g(1); return 0; }
  void g(int);
  int x = f<int>();
  ```

- An unqualified name is found in a dependent base class, where the
  standard requires `this->`.
- The body of a template that is never instantiated is not checked; an
  undeclared name in it is not diagnosed.

Substitution failure (SFINAE), partial ordering of function templates,
deduction through alias templates and constraint checking follow the
standard.

### Constant evaluation

Where a constant is needed (`static_assert`, an array bound, a template
argument, an enumerator, a `case` label, `if constexpr`, a `consteval`
call), an expression that does not fold is run by an interpreter.
It handles loops, recursion, references, classes with constructors and
virtual functions, lambdas, strings, bit-fields, floating point with the
target's `long double` format, and `__int128` in 128 bits.

Limits:

- At most 20,000,000 evaluation steps and a call depth of 512.
- Not evaluated: `new` and `delete`, a change of a union's active
  member, virtual base classes, and `__builtin_bit_cast`. An expression
  that uses them is not a constant expression.
- Destructors do not run during constant evaluation. A constant
  expression whose value depends on a destructor's side effect gets a
  different value than the same code at run time.

Static initialization:

- A namespace-scope or local `static` object of scalar type whose
  initializer evaluates to a constant is written as static data and
  needs no guard, even when the initializer calls `constexpr` functions.
- An object of class type initialized by a constructor, including a
  `constexpr` object, is initialized at run time, before `main`, by the
  unit's dynamic initializer. Its value is still available in constant
  expressions. g++ initializes such an object statically.

### Access control

`private` and `protected` are enforced for data members, member
functions, constructors, and conversions to a base class:

```text
error: 'priv' is private in 'A'
error: 'pf' is protected in 'A'
error: 'A' is an inaccessible base of 'B'
error: the constructor of 'K' selected here is private
```

Where EmbCC cannot decide, it grants access. These checks are not made:

- Access during template argument substitution. Type traits such as
  `std::is_constructible` answer as though every member were public.
- The rule of [class.protected] that a protected member be named
  through an object of the derived class.
- Access to nested types and member typedefs.
- The destructor called implicitly at the end of an object's lifetime.
- A member that a using-declaration republishes with different access.
- Friendship granted to a function template or a specialization
  befriends every function.

`-fno-access-control` turns the checks off.

### Checks EmbCC does not make

These ill-formed constructs are accepted without a diagnostic:

- Overriding a `final` function, deriving from a `final` class, `override`
  on a function that overrides nothing, and an overrider whose return
  type differs from the overridden function's.
- Narrowing conversions in list-initialization.
- A `constinit` variable whose initializer is not constant.
- Discarding the result of a `[[nodiscard]]` function; using a
  `[[deprecated]]` entity.
- Dynamic exception specifications and `register` in C++17 and later.
- Errors in uninstantiated templates (see above).

## GNU extensions in C++

These GNU extensions are available in C++: statement expressions,
`__typeof__` and `__typeof`, `__attribute__`, extended `asm` statements,
case ranges, `__int128`, `__complex__` types with `__real__` and
`__imag__`, variable-length arrays of scalar type, flexible array members,
`__builtin_*` functions, and asm labels on declarations.

These are refused in C++:

| Extension | Diagnostic |
|---|---|
| `asm goto` | `asm goto in C++ is not supported yet` |
| File-scope `asm` | `file-scope asm in C++ is not supported yet` |
| Label addresses (`&&label`) | `label addresses are not supported in C++` |
| Computed `goto` | `computed goto is not supported in C++` |
| `_Atomic` | `_Atomic is not supported in C++` |
| A variable-length array of class type | `a variable-length array of S is not supported` |
| `sizeof` of a variable-length array | `sizeof a variable-length array is not supported` |
| Imaginary constants (`2.0i`) | `no literal operator 'operator""i' for this literal` |
| `typeof` without underscores | `expected a declaration before 'typeof'` |
| `?:` with the middle operand omitted | `expected an expression before ':'` |
| Array designators (`[1] = 2`) | `expected a capture` |
| `__label__` | `'__label__' was not declared in this scope` |

The general list is in [Extensions](extensions.md).

## Predefined macros

### Compiler identity

A C++ unit presents itself as g++ 16.2, so that libstdc++ and newlib take
their GCC paths. C units do not define these.

| Macro | Value |
|---|---|
| `__GNUC__`, `__GNUG__` | `16` |
| `__GNUC_MINOR__` | `2` |
| `__GNUC_PATCHLEVEL__` | `0` |
| `__VERSION__` | `"16.2.0 (EmbCC)"` |
| `__GXX_RTTI` | `1` |
| `__GNUC_STDC_INLINE__` | `1` |
| `__SIZEOF_INT128__` | `16` |
| `__GLIBCXX_TYPE_INT_N_0`, `__GLIBCXX_BITSIZE_INT_N_0` | `__int128`, `128` (not in the strict `c++NN` modes) |
| `__STDCPP_DEFAULT_NEW_ALIGNMENT__` | `16` (C++17 and later) |
| `__EXCEPTIONS` | `1` (not under `-fno-exceptions`) |
| `__STRICT_ANSI__` | `1` (the `c++NN` modes) |
| `__EMBCC__` | `1` |

`__STDC_VERSION__` is not defined in C++.

### Feature-test macros

EmbCC defines a feature-test macro only for a feature it implements, from
the standard in which g++ first defines it, at g++'s value for the
selected standard. The values in the default mode (C++20):

| Macro | Value | Defined from |
|---|---|---|
| `__cpp_aggregate_nsdmi` | `201304L` | C++14 |
| `__cpp_aggregate_paren_init` | `201902L` | C++20 |
| `__cpp_alias_templates` | `200704L` | C++11 |
| `__cpp_aligned_new` | `201606L` | C++17 |
| `__cpp_attributes` | `200809L` | C++11 |
| `__cpp_binary_literals` | `201304L` | C++98 |
| `__cpp_capture_star_this` | `201603L` | C++17 |
| `__cpp_char8_t` | `202207L` | C++20 (earlier with `-fchar8_t`) |
| `__cpp_concepts` | `201907L` | C++20 |
| `__cpp_conditional_explicit` | `201806L` | C++20 |
| `__cpp_constexpr` | `201603L` | C++11 (`200704L`), C++14 (`201304L`) |
| `__cpp_constexpr_dynamic_alloc` | `201907L` | C++20 |
| `__cpp_decltype` | `200707L` | C++11 |
| `__cpp_decltype_auto` | `201304L` | C++14 |
| `__cpp_deduction_guides` | `201703L` | C++17 |
| `__cpp_delegating_constructors` | `200604L` | C++11 |
| `__cpp_enumerator_attributes` | `201411L` | C++17 |
| `__cpp_exceptions` | `199711L` | C++98 (not under `-fno-exceptions`) |
| `__cpp_fold_expressions` | `201603L` | C++17 |
| `__cpp_generic_lambdas` | `201304L` | C++14 |
| `__cpp_guaranteed_copy_elision` | `201606L` | C++17 |
| `__cpp_hex_float` | `201603L` | C++98 |
| `__cpp_if_constexpr` | `201606L` | C++17 |
| `__cpp_impl_coroutine` | `201902L` | C++20 |
| `__cpp_impl_three_way_comparison` | `201907L` | C++20 |
| `__cpp_init_captures` | `201304L` | C++14 |
| `__cpp_initializer_lists` | `200806L` | C++11 |
| `__cpp_inline_variables` | `201606L` | C++17 |
| `__cpp_lambdas` | `200907L` | C++11 |
| `__cpp_namespace_attributes` | `201411L` | C++17 |
| `__cpp_nested_namespace_definitions` | `201411L` | C++17 |
| `__cpp_nsdmi` | `200809L` | C++11 |
| `__cpp_range_based_for` | `201603L` | C++11 (`200907L`) |
| `__cpp_ref_qualifiers` | `200710L` | C++11 |
| `__cpp_return_type_deduction` | `201304L` | C++14 |
| `__cpp_rtti` | `199711L` | C++98 (also under `-fno-rtti`) |
| `__cpp_rvalue_references` | `200610L` | C++11 |
| `__cpp_sized_deallocation` | `201309L` | C++14 |
| `__cpp_static_assert` | `201411L` | C++11 (`200410L`) |
| `__cpp_structured_bindings` | `201606L` | C++17 |
| `__cpp_threadsafe_static_init` | `200806L` | C++98 |
| `__cpp_unicode_characters` | `200704L` | C++11 |
| `__cpp_unicode_literals` | `200710L` | C++11 |
| `__cpp_user_defined_literals` | `200809L` | C++11 |
| `__cpp_using_enum` | `201907L` | C++20 |
| `__cpp_variable_templates` | `201304L` | C++14 |
| `__cpp_variadic_templates` | `200704L` | C++11 |

`__cpp_constexpr_dynamic_alloc` is defined although dynamic allocation
is not evaluated in constant expressions; libstdc++ declares
`std::construct_at` only when it is defined. These are not defined:
`__cpp_consteval`, `__cpp_constinit`, `__cpp_designated_initializers`,
`__cpp_modules`, `__cpp_nontype_template_args`,
`__cpp_noexcept_function_type`, `__cpp_nontype_template_parameter_auto`,
`__cpp_aggregate_bases`, `__cpp_if_consteval`,
`__cpp_explicit_this_parameter`, `__cpp_static_call_operator`,
`__cpp_size_t_suffix` and `__cpp_multidimensional_subscript`.

`__has_builtin` answers for the builtin functions and the type-trait
intrinsics of the C++ front end (`__is_class`, `__is_same`,
`__is_base_of`, `__is_constructible`, `__is_trivially_copyable`,
`__underlying_type`, `__remove_cvref`, `__make_integer_seq`, ...).

## The C++ ABI

EmbCC follows the Itanium C++ ABI on every target, as g++ does on the
x86-64 and AArch64 ELF targets.

- **Names.** Mangling follows the Itanium ABI, including the standard
  substitutions (`St`, `Sa`, `Ss`, ...) and ABI tags (`B5cxx11`). The
  external names in an object are those g++ and Clang give the same
  declarations.
- **Classes.** Layout follows the Itanium algorithm: the vptr, primary
  bases, empty bases at offset 0, reuse of a non-POD base's tail
  padding, virtual bases after the non-virtual part, and
  `[[no_unique_address]]`.
- **Virtual functions.** One vtable group per dynamic class, with
  secondary vtables, `this`-adjusting and virtual thunks, construction
  vtables and VTTs. The vtable is emitted in the unit that defines the
  key function, or in every unit that needs it when there is none.
  Constructors and destructors have their complete-object, base-object
  and (for a virtual destructor) deleting variants.
- **RTTI.** `type_info` objects are those of the `__cxxabiv1` classes
  (`__class_type_info`, `__si_class_type_info`, `__vmi_class_type_info`,
  `__pointer_type_info`, ...); `dynamic_cast` calls `__dynamic_cast`.
- **Calls.** A class that is not trivially copyable is passed as the
  address of a temporary the caller builds and destroys, and returned
  through a result slot the caller passes (in `x8` on AArch64).
- **Pointers to member functions** are `{ptr, adj}` pairs. On x86-64 a
  virtual function's `ptr` is its vtable offset plus 1; on AArch64 it
  follows the ARM variant g++ uses there: `ptr` is the vtable offset, and
  `adj` is twice the adjustment, plus 1 for a virtual function.
- **Exceptions.** `throw` calls `__cxa_allocate_exception` and
  `__cxa_throw`; handlers call `__cxa_begin_catch` and `__cxa_end_catch`;
  cleanups end in `_Unwind_Resume`. Every function has a CIE/FDE in
  `.eh_frame`, and functions with handlers or cleanups have an LSDA read
  by `__gxx_personality_v0`. On Darwin the unwind information is written
  as `__LD,__compact_unwind` entries and the LSDA in
  `__TEXT,__gcc_except_tab`.
- **Static initialization.** Each unit has one initialization function,
  listed in `.init_array`. Destructors of namespace-scope objects are
  registered with `__cxa_atexit`. A function-local static is guarded with
  `__cxa_guard_acquire`, `__cxa_guard_release` and `__cxa_guard_abort`;
  in an inline function the static and its guard are weak symbols under
  their Itanium names, so all units share one.
- **Vague linkage.** Inline functions, template instances, implicit
  special members, vtables and `type_info` objects are emitted as weak
  definitions, in every unit that uses them. EmbCC does not use COMDAT
  section groups.

## Run-time support

### What a C++ object refers to

| Construct | Symbols referenced |
|---|---|
| `new`, `delete` and their array forms | `_Znwm`, `_Znam`, `_ZdlPv`, `_ZdlPvm`, `_ZdaPv` |
| A pure virtual function | `__cxa_pure_virtual` |
| A namespace-scope object with a destructor | `__cxa_atexit`, `__dso_handle` |
| A local `static` with a dynamic initializer | `__cxa_guard_acquire`, `__cxa_guard_release`; with exceptions also `__cxa_guard_abort` |
| A polymorphic class (with RTTI) | the vtables of `__cxxabiv1::__class_type_info` and its derived classes |
| `typeid` of a fundamental type, `catch (int)` | `_ZTIi` and the other fundamental `type_info` objects |
| `dynamic_cast` | `__dynamic_cast` |
| Landing pads (exceptions on) | `__gxx_personality_v0`, `_Unwind_Resume`, `__cxa_call_terminate` |
| `throw`, `catch`, `throw;` | `__cxa_allocate_exception`, `__cxa_throw`, `__cxa_begin_catch`, `__cxa_end_catch`, `__cxa_rethrow` |

`__cxa_atexit` and `__cxa_finalize` belong to the C library. The rest
come from a C++ runtime: EmbCC's `libcxx.a`, GCC's libsupc++, or the
platform's. The unwinder (`_Unwind_*`) comes from libgcc on bare metal
and EmbLinkOS, from EmbCC's `librt.a` on the `-linux-gnu` targets, and
from the system on Darwin.

### EmbCC's C++ library

`lib/libcxx` is EmbCC's C++ runtime and standard library, compiled by
EmbCC. Its headers are searched before the C library's in a C++ unit
(`--print-search-dirs` lists them). `libcxx.a` holds `operator new` and
`operator delete` in all their forms, the static-local guards (thread-safe
through the C library's futex interface), `std::type_info` and the
`__cxxabiv1` type-information classes, `__dynamic_cast`, the
`__cxa_*` exception layer with `__gxx_personality_v0`,
`std::exception_ptr`, `std::terminate`, and the compiled parts of the
standard library.

The headers cover the C++20 standard library except `<locale>`. See
[Libraries](libraries.md) for the list, the design and the limits.

When EmbCC runs from its build tree, the directory next to the `embcc`
binary (`include/`, the freestanding headers) is searched before
`lib/libc/include`, and its declarations-only `<string.h>` hides the C
library's. `<cstring>`, and every header that includes it (`<string>`,
`<iostream>`, `<map>`, ...), then fails:

```text
error: 'strcoll' is not declared in '::'
```

Add `-I lib/libc/include` (relative to the EmbCC tree) in that case. An
installed EmbCC does not have this problem.

### GCC's libstdc++

In a C++ unit EmbCC presents itself as g++ 16.2 and implements the
builtins and type-trait intrinsics that libstdc++ asks for with
`__has_builtin`. To use libstdc++ instead of EmbCC's library, put its
include directories and the C library's ahead of EmbCC's with `-I`, for
example for a GCC 16.2 `x86_64-elf` toolchain over newlib:

```sh
embcc --target=x86_64-elf \
      -I$GXX/x86_64-elf/include/c++/16.2.0 \
      -I$GXX/x86_64-elf/include/c++/16.2.0/x86_64-elf \
      -I$NEWLIB/x86_64-elf/include \
      -c prog.cc -o prog.o
```

and link `prog.o` with g++'s `libstdc++.a`, `libsupc++.a` and
`libgcc.a`. EmbCC's test suite does this for its C++ programs on
`x86_64-elf` and `aarch64-elf` and compares their output with g++'s
builds. It also builds libstdc++ and libsupc++ themselves from GCC's
sources with EmbCC (`make test-libstdcxx`).

### Freestanding programs

On a bare-metal target nothing runs the C++ start-up for you. The
start-up code must:

- call each function pointer in `.init_array`, in order, before `main`
  (namespace-scope constructors);
- provide `__cxa_atexit` and `__dso_handle` if namespace-scope objects
  have destructors;
- with libgcc's unwinder, register the unwind tables before the first
  `throw`, by calling `__register_frame_info` with the start of
  `.eh_frame` (EmbCC supplies no `crtbegin.o` that would do it).

A program built with `-fno-exceptions -fno-rtti` needs no C++ runtime
beyond the symbols in the table above that its code refers to. See
[Embedded programming](embedded.md).

## Debugging C++

Compile with `-g`. Breakpoints and stepping use the C++ source lines.
Functions are named by their mangled names, which a debugger demangles
if it is told the language is C++. Variables and members appear as in the
lowered C: a class is a structure named `_C<length><name>`, a base class
subobject is a byte array member `__cx_base0`, `__cx_base1`, ..., and the
vptr is the member `__cx_vptr`. `--emit-c` shows the lowered C that the
debug information describes. See [Debugging](debugging.md).
