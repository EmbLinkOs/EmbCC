# C Language Support

This page describes the C language EmbCC accepts: the `-std=` values and
what they change, the predefined macros that describe the language, and
each feature of C89, C99, C11, C17 and C23 with its status. For every
feature that is partial or refused it quotes the diagnostic EmbCC prints,
so that the message can be searched for. It is for people porting C code
to EmbCC and for people deciding which constructs a portable program can
use. GNU and Clang extensions (statement expressions, `__attribute__`,
builtins, pragmas) are in [Extensions](extensions.md). Choices the
standard leaves to the implementation, such as the signedness of `char`,
are in [Implementation-defined behavior](implementation-defined.md). C++
is in [C++ support](cxx.md).

## Summary

EmbCC compiles one C dialect:

- the C17 language, which is C11 with the corrections of ISO/IEC 9899:2018;
- the subset of C23 listed under [C23](#c23);
- the GNU extensions described in [Extensions](extensions.md).

`-std=` does not select a different dialect, and `__STDC_VERSION__` is
`201710L` whatever it names.

As a rule, a construct EmbCC does not implement is refused with an error
that names it. The exceptions are the places where EmbCC accepts a
program the standard requires it to diagnose, or gives a program a
meaning other than the standard's. Check these first when porting code:

- Struct, union and enum tags, typedef names and enumeration constants
  declared in a block remain visible to the end of the translation unit.
  See [Scope of tags, typedefs and enumeration constants](#scope-of-tags-typedefs-and-enumeration-constants).

EmbCC is also stricter than the standard in two places that commonly
affect existing code: a non-`void` function other than `main` that can
reach its closing brace is an error ([details](#functions-that-can-reach-their-closing-brace)),
and an empty parameter list `()` always means "no parameters"
([details](#empty-parameter-lists)).

Features whose availability depends on the target (variable length
arrays on AVR, `long double`, `__int128`, atomic sizes, thread-local
storage) are in [Target-dependent features](#target-dependent-features).

The tables below use three statuses:

| Status | Meaning |
|---|---|
| Supported | Implemented as the standard specifies. |
| Partial | Implemented, with the restrictions given in the Notes column. |
| Not supported | Refused with an error, quoted in the Notes column. |

Diagnostics are quoted without their `embcc: FILE:LINE:COL: error:`
prefix. The format is described in [Diagnostics](diagnostics.md).

## Selecting a standard

### `-std=STANDARD`

Name a C standard. The option is accepted so that existing build systems
work; it does not change what EmbCC accepts or the macros it defines.

| `-std=` value | Standard | Result |
|---|---|---|
| `c89`, `c90`, `iso9899:1990`, `gnu89`, `gnu90` | C89/C90 | Accepted with a warning |
| `c99`, `c9x`, `iso9899:1999`, `gnu99`, `gnu9x` | C99 | Accepted with a warning |
| `c11`, `c1x`, `iso9899:2011`, `gnu11`, `gnu1x` | C11 | Accepted |
| `c17`, `c18`, `iso9899:2017`, `gnu17`, `gnu18` | C17 | Accepted |
| `c23`, `c2x`, `gnu23`, `gnu2x` | C23 | Accepted |
| any other value | | Error |

A standard older than C11 prints:

```text
embcc: warning: -std=c99 is accepted but not enforced; EmbCC has one C dialect, C11 with the GNU extensions, and will compile newer constructs anyway
```

Any other value is an error, for example `-std=c2y`, `-std=gnu2y`,
`-std=iso9899:199409` and `-std=iso9899:2018`:

```text
embcc: error: unknown standard '-std=c2y'
```

Whatever the value, `__STDC_VERSION__` is `201710L` and `__STRICT_ANSI__`
is not defined. With no `-std=` option the dialect is the same. EmbCC has
no mode that diagnoses extensions. `-ansi` is refused as an unknown
argument. `-pedantic` and `-pedantic-errors` are accepted, turn nothing
on, and print a warning that says so:

```text
embcc: warning: -pedantic: EmbCC has no diagnostics for extensions to ISO C, so this turns nothing on
```

See [Diagnostics](diagnostics.md).

The C++ values (`c++20`, `gnu++17`, ...) are described in
[Invoking EmbCC](invoking.md#-stdstandard) and [C++ support](cxx.md).

### Keywords

Because there is one dialect, the C23 keywords are recognized in every
compilation. `alignas`, `alignof`, `nullptr`, `static_assert`,
`thread_local`, `typeof` and `typeof_unqual` are reserved and cannot be
used as identifiers. `auto`, `bool`, `constexpr`, `false` and `true` are
recognized from context, so code that declares its own entity of one of
those names keeps its meaning:

```c
typedef int bool;        /* accepted: bool is this typedef     */
enum { false, true };    /* accepted: true is this enumerator  */
int constexpr = 1;       /* accepted: constexpr is a variable  */
```

The GNU keywords (`asm`, `__typeof__`, `__attribute__`, ...) are in
[Extensions](extensions.md).

### Testing for features in source code

- `#if __STDC_VERSION__ >= 202311L` is false, although many C23 features
  are available. Test for the feature, or for `__EMBCC__`.
- `__has_c_attribute(NAME)` is defined, but evaluates to 0 for every
  name, including the standard attributes EmbCC accepts.
- `__has_include` works as specified.
- None of the `__STDC_NO_ATOMICS__`, `__STDC_NO_COMPLEX__`,
  `__STDC_NO_THREADS__` or `__STDC_NO_VLA__` macros is defined. Where a
  target cannot compile one of these features, the compilation fails
  with the error given under
  [Target-dependent features](#target-dependent-features).

## Predefined macros

The macros the C standard defines. The target's macros (`__x86_64__`,
`__SIZEOF_INT128__`, ...) are listed by `--dump-predef` and described in
[Targets](targets.md). EmbCC also defines `__EMBCC__` as `1`; it does not
define `__GNUC__` when compiling C.

| Macro | Value |
|---|---|
| `__STDC__` | `1` |
| `__STDC_VERSION__` | `201710L`, for every `-std=` value |
| `__STDC_HOSTED__` | `1`, also with `-ffreestanding` |
| `__FILE__`, `__LINE__` | Supported; `#line` changes both |
| `__DATE__`, `__TIME__` | Supported; `SOURCE_DATE_EPOCH` sets them, as in GCC. See [Implementation-defined behavior](implementation-defined.md#preprocessing-directives). |
| `__STDC_IEC_559__`, `__STDC_IEC_559_COMPLEX__` | Not defined |
| `__STDC_ISO_10646__`, `__STDC_MB_MIGHT_NEQ_WC__`, `__STDC_UTF_16__`, `__STDC_UTF_32__` | Not defined |
| `__STDC_ANALYZABLE__`, `__STDC_LIB_EXT1__` | Not defined |
| `__STDC_NO_ATOMICS__`, `__STDC_NO_COMPLEX__`, `__STDC_NO_THREADS__`, `__STDC_NO_VLA__` | Not defined |

## C89 and C90

| Feature | Status | Notes |
|---|---|---|
| Declarations, expressions and statements | Supported | Except as listed in this table and under [Feature notes](#feature-notes). |
| Function prototypes | Supported | |
| Empty parameter list `int f();` | Partial | Means "no parameters", as in C23. See [Empty parameter lists](#empty-parameter-lists). |
| Old-style (K&R) function definitions | Not supported | `int add(a, b) int a, b; { ... }` gives `expected a parameter type before 'a'`. C23 removed them. |
| Implicit `int` | Not supported | `static x = 3;` gives `expected a type before 'x'`. C99 removed it. |
| Implicit function declarations | Not supported | `'foo' is not declared in 'main' — for a call, add a prototype or define it first [E0001]`. C99 removed them. |
| Reaching the `}` of a non-`void` function | Not supported | An error even when the value is never used, except in `main`: `control may reach the end of 'f' — every path must end in a return statement [E0008]`. See [Functions that can reach their closing brace](#functions-that-can-reach-their-closing-brace). |
| `return;` in a non-`void` function | Not supported | `'g' returns int; 'return' needs a value`. A constraint violation since C99. |
| `auto` storage class | Supported | `auto int x;` |
| `register` storage class | Partial | Accepted on block-scope objects. On a parameter or at file scope: `'register' is not supported yet (see docs/manual/c-language.md)`. Taking the address of a `register` object is not diagnosed. |
| `const` | Supported | See [Const qualification](#const-qualification). |
| `volatile` | Supported | |
| Scopes | Partial | Objects, functions and labels follow the standard's rules. Tags, typedef names and enumeration constants do not. See [Scope of tags, typedefs and enumeration constants](#scope-of-tags-typedefs-and-enumeration-constants). |
| Structures, unions, bit-fields | Supported | A structure or union with no members is refused: `a struct/union needs at least one member`. |
| Enumerations | Supported | A constant outside the range of `int` is accepted without a diagnostic and gives the enumeration a wider type, as in C23. See [Enumeration constants](#enumeration-constants). |
| Tentative definitions | Partial | A file-scope array of unknown size, `int a[];`, is refused even when a later declaration completes it: `'a' has incomplete type int[0]`. `extern int a[];` is accepted. |
| `goto`, labels, `switch` | Supported | |
| Adjacent string literal concatenation | Supported | A prefixed and an unprefixed literal concatenate: `L"wi" "de"` is `L"wide"`. |
| Wide literals `L"..."`, `L'x'` | Supported | |
| Multi-character constants `'ab'` | Not supported | `a character constant holds one character (multi-character constants are not supported)` |
| Trigraphs `??=` `??(` ... | Not supported | Not replaced: `"??="` is a three-character string. C23 removed them. |

### C95 (Amendment 1)

| Feature | Status | Notes |
|---|---|---|
| Digraphs `<:` `:>` `<%` `%>` `%:` `%:%:` | Not supported | Read as separate punctuators. `int a<:2:>;` gives `expected ';' before '<' [E0002]`; a `%:define` line is not a directive. |
| `<iso646.h>` | Supported | |
| `<wchar.h>`, `<wctype.h>` | Supported | Provided by the C library; see [Libraries](libraries.md). |

## C99

| Feature | Status | Notes |
|---|---|---|
| `//` comments | Supported | A backslash at the end of a `//` comment does not continue the comment onto the next line; see [Translation phases](#translation-phases). |
| Declarations mixed with statements; declarations in `for` | Supported | |
| `long long`, `unsigned long long`, `LL` and `ULL` suffixes | Supported | |
| `_Bool` and `<stdbool.h>` | Supported | |
| `inline` | Yes | See [Inline functions](#inline-functions). |
| `restrict` | Supported | Accepted. EmbCC does not use it for optimization. |
| Variable length arrays | Partial | See [Variable length arrays](#variable-length-arrays). Not available on AVR. |
| Variably modified types: pointers to VLAs, VLA parameters `int a[n][m]`, `[*]` | Supported | |
| `static` and qualifiers in array parameters, `int a[static 4]` | Not supported | `expected an expression, got 'static'` (likewise `got 'const'`, `got 'restrict'`). |
| Flexible array members | Supported | An initializer for the flexible member is accepted only on a static object, which is a GNU extension. On an automatic object: `'l' is not static, so its flexible array member 'd' cannot be initialized: the object has no room for the elements`. |
| Designated initializers | Supported | |
| Compound literals | Partial | At file scope, an array compound literal that converts to a pointer is refused: `int *p = (int[]){ 1, 2, 3 };` gives `a scalar takes exactly one initializer`. `&(int){ 3 }` at file scope, structure compound literals and array compound literals in a block work. Storage-class specifiers in a compound literal are C23; see the [C23](#c23) table. |
| Non-constant initializers for automatic aggregates | Supported | |
| `_Complex` and `<complex.h>` | Partial | `float`, `double` and `long double` complex types. Static initializers are limited. See [Complex types](#complex-types). |
| `_Imaginary` | Not supported | Not a keyword; a declaration that uses it is a syntax error. |
| Hexadecimal floating constants `0x1.8p1` | Supported | |
| Universal character names | Partial | In character constants and string literals. In an identifier one is refused, `int caf\u00e9;` gives `character '\' is not supported yet`; write the character in UTF-8 instead. |
| `__func__` | Supported | |
| Variadic macros, `__VA_ARGS__` | Supported | |
| Empty macro arguments | Supported | |
| `_Pragma` operator | Supported | |
| `#pragma STDC FP_CONTRACT`, `FENV_ACCESS`, `CX_LIMITED_RANGE` | Partial | Accepted and ignored. |
| `va_copy` | Supported | |
| Trailing comma in an enumerator list | Supported | |
| `main` returns 0 when it reaches `}` | Supported | |
| Integer division truncates toward zero | Supported | |
| `__STDC_IEC_559__` and the other conditional feature macros | Not supported | Not defined; see [Predefined macros](#predefined-macros). |

## C11

| Feature | Status | Notes |
|---|---|---|
| `_Alignas`, `_Alignof`, `<stdalign.h>` | Partial | See [Alignment specifiers](#alignment-specifiers). |
| `_Noreturn`, `<stdnoreturn.h>` | Supported | The same as `__attribute__((noreturn))`. |
| `_Static_assert` | Supported | A failed assertion: `static assertion failed: MESSAGE`. A non-constant condition: `_Static_assert needs a constant integer expression`. |
| `_Generic` | Supported | See [Generic selection](#generic-selection). With no matching association and no `default`: `no _Generic association matches type double`. |
| `_Atomic` qualifier and `_Atomic(T)` specifier | Partial | Integer and pointer types only; sizes depend on the target. See [Atomic types](#atomic-types). |
| `<stdatomic.h>` | Supported | See [Atomic types](#atomic-types). |
| `_Thread_local` | Partial | Per-thread storage on the x86-64 and AArch64 ELF, EmbLinkOS and Linux targets; refused on macOS and Windows; one shared instance on Cortex-M, RISC-V, MIPS32 and AVR. See [Target-dependent features](#target-dependent-features). At block scope without `static` or `extern`: `a block-scope __thread object must also be static: an automatic one is already private to the call`. |
| Anonymous structures and unions | Supported | |
| `char16_t`, `char32_t`, `u"..."`, `U"..."`, `u'x'`, `U'x'`, `u8"..."` | Supported | `<uchar.h>` is provided by the C library. |
| `max_align_t` | Supported | Aligned as `long double` or `long long`, whichever is stricter: 16 bytes on x86-64, AArch64 and RISC-V, 8 on Cortex-M and MIPS32, 1 on AVR. |
| `CMPLX`, `CMPLXF`, `CMPLXL` | Partial | Defined as function calls, so they cannot initialize a static object. See [Complex types](#complex-types). |
| Extended identifiers (C11 Annex D) | Supported | Written as UTF-8; a universal character name in an identifier is refused (see the [C99](#c99) table). Any other non-ASCII byte outside a literal: `byte 0xc3 is not part of a character C allows here (identifiers take UTF-8 letters, C11 Annex D)`. |
| Optional features: VLAs, complex types, atomics, threads | Supported | All four are provided, subject to [Target-dependent features](#target-dependent-features). |

## C17

C17 adds no language features; it corrects C11. `__STDC_VERSION__` has
C17's value, `201710L`.

## C23

| Feature | Status | Notes |
|---|---|---|
| `bool`, `true`, `false` keywords | Supported | `true` and `false` have type `bool`. After `#include <stdbool.h>` they are the macros `1` and `0`, of type `int`. |
| `alignas`, `alignof`, `static_assert`, `thread_local` keywords | Supported | |
| `static_assert` with one operand | Supported | A failed assertion: `static assertion failed: (no message)`. |
| `nullptr` | Partial | Its type is `void *`, not `nullptr_t`. `<stddef.h>` does not declare `nullptr_t`: `expected a type before 'nullptr_t'`. |
| `constexpr` objects | Partial | Integer types only. See [Constexpr objects](#constexpr-objects). |
| `auto` type inference | Partial | At block scope only. See [Type inference](#type-inference). |
| `typeof`, `typeof_unqual` | Partial | Not every expression form. See [Type inference](#type-inference). |
| Attributes `[[...]]` | Partial | Not in every position. See [Attributes](#attributes). |
| `[[deprecated]]` | Supported | A use warns: `'g' is deprecated [-Wdeprecated-declarations]`. A message argument is accepted and not printed. |
| `[[fallthrough]]` | Supported | Accepted. EmbCC does not warn about fall-through. |
| `[[maybe_unused]]` | Supported | Suppresses `-Wunused-variable`, `-Wunused-function` and, on a parameter, `-Wunused-parameter` for the declaration. |
| `[[nodiscard]]` | Supported | Discarding the result warns: `result of 'f' is discarded, and it is declared warn_unused_result [-Wunused-result]`. A message argument is accepted and not printed. |
| `[[noreturn]]`, `[[_Noreturn]]` | Supported | |
| `[[unsequenced]]`, `[[reproducible]]` | Not supported | Ignored with a warning: `attribute 'unsequenced' is not one EmbCC knows, and is ignored [-Wattributes]`. |
| `__has_c_attribute` | Partial | Defined, and evaluates to 0 for every attribute. |
| `__has_include` | Supported | |
| `#elifdef`, `#elifndef` | Supported | |
| `#warning` | Supported | Prints `warning: #warning: TEXT`. |
| `#embed` | Partial | Without parameters only. See [Embedding binary data](#embedding-binary-data). |
| `__has_embed` | Not supported | Not defined. In `#if` it is an unknown identifier followed by parentheses: `trailing junk in #if expression`. |
| `__VA_OPT__` | Supported | |
| `true` and `false` in `#if` | Supported | |
| Binary constants `0b1010` | Supported | Also in `#if`. |
| Digit separators `1'000'000` | Supported | In integer and floating constants, and in `#if`. |
| `u8` character constants `u8'a'` | Not supported | Read as the identifier `u8` followed by `'a'`, which is a syntax error such as `expected ';' before number 97 [E0002]`. |
| `char8_t`; `u8"..."` of type `unsigned char[]` | Not supported | `u8"..."` has C17's type, `char[]`. `<uchar.h>` does not declare `char8_t`. |
| `_BitInt(N)`, the `wb` and `uwb` suffixes | Not supported | `expected a type before '_BitInt'`; a `3wb` constant gives `malformed integer constant`. |
| Enumerations with a fixed underlying type, `enum E : unsigned char` | Partial | The enumeration and its constants have the underlying type, and `enum E : short;` declares it ahead of its definition. A value the underlying type cannot represent is not diagnosed. See [Enumeration constants](#enumeration-constants). A non-integer type: `an enum's underlying type must be an integer type`. |
| Enumeration constants outside the range of `int` | Partial | The enumeration and its constants take a type that can represent every value. A value of 2^63 or more is read as negative. See [Enumeration constants](#enumeration-constants). |
| Empty initializer `= {}` | Partial | Zero-initializes any object except a VLA, which is refused: `variable length array 'a' cannot be initialized`. See [Initializers](#initializers). |
| Labels before declarations and at the end of a compound statement | Supported | |
| Unnamed parameters in a function definition | Not supported | `parameter 1 of 'f' needs a name in a definition` |
| Redefinition of a tag with the same content | Not supported | `redefinition of 'P'` |
| Storage-class specifiers in compound literals, `(static int[]){ 1 }` | Not supported | `expected an expression, got 'static'` |
| `()` means `(void)`; K&R definitions removed | Supported | See [Empty parameter lists](#empty-parameter-lists). |
| Trigraphs removed | Supported | EmbCC never replaces trigraphs. |
| `_Decimal32`, `_Decimal64`, `_Decimal128` | Not supported | `expected a type before '_Decimal32'` |
| `_Float32`, `_Float64` | Partial | The same types as `float` and `double`. The `f32` and `f64` constant suffixes are refused: `malformed floating constant`. |
| `_Float128` | Partial | The same type as `long double` where that is IEEE binary128. See [Target-dependent features](#target-dependent-features). |
| `_Float16` | Not supported | `` _Float16/__fp16 is not supported: EmbCC has no 16-bit floating-point type, and widening it to `float` would give 24 bits of mantissa where the program asked for 11 `` |
| `_Float32x`, `_Float64x` | Not supported | `expected a type before '_Float32x'` |
| `unreachable()` in `<stddef.h>` | Not supported | Not defined: `'unreachable' is not declared in 'f' — for a call, add a prototype or define it first [E0001]`. `__builtin_unreachable()` is available. |
| Width macros `INT_WIDTH`, `INT8_WIDTH`, ... | Not supported | Not defined by `<limits.h>` or `<stdint.h>`. |
| `__STDC_VERSION__` of `202311L` | Not supported | The value is `201710L`. |
| Identifiers per Unicode UAX #31 | Not supported | Identifiers use the C11 Annex D character ranges. |

### Later drafts

The following features of the C2y working draft are not supported:

| Feature | Diagnostic |
|---|---|
| `_Countof(a)` | `'_Countof' is not declared in 'f' — for a call, add a prototype or define it first [E0001]` |
| Octal prefix `0o17` | `malformed integer constant` |
| Delimited escapes `'\o{101}'` | `unknown escape '\o' in a literal` |
| Declarations in `if` and `switch`, `if (int x = g(); x > 0)` | `expected an expression, got 'int'` |
| A type name as the controlling operand of `_Generic` | `expected an expression, got 'int'` |

Case ranges, `case 1 ... 3:`, are accepted as a GNU extension; see
[Extensions](extensions.md).

## Target-dependent features

The language features whose availability depends on the target. The
targets are described in [Targets](targets.md): "x86-64" covers the ELF,
EmbLinkOS and Linux triples, "AArch64" covers the ELF, EmbLinkOS and
Linux triples, and "Cortex-M" covers every `thumb*` triple. Every compile
for the Windows target also prints the `-Wwindows-abi` warning described
there.

| Feature | x86-64 | x86-64 macOS | x86-64 Windows | AArch64 | Apple arm64 | Cortex-M | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|---|---|---|
| Variable length arrays | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes | No |
| `long double` format | x87 80-bit, 16 bytes | x87 80-bit, 16 bytes | x87 80-bit, 16 bytes | binary128, 16 bytes | binary64, 8 bytes | binary64, 8 bytes | binary128, 16 bytes | binary128, 16 bytes | binary32, 4 bytes |
| `long double` arithmetic | Yes | Yes | Not as a parameter or result | Yes, in software | Yes | Yes | No | No | Yes |
| `float` and `double` `_Complex` | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| `long double _Complex` | Yes | Yes | No | Yes | Yes | Yes | No | No | Yes |
| `__int128` | Yes | Yes | Not in a function signature | Yes | Yes | No | No | Declarations and `sizeof` only | No |
| `_Float128` | No | No | No | Yes | No | No | Declarations and `sizeof` only | Declarations and `sizeof` only | No |
| `_Atomic` operators | 1 to 8 bytes | 1 to 8 bytes | 1 to 8 bytes | 1 to 8 bytes | 1 to 8 bytes | 1 to 4 bytes | Load and store 1 to 4 bytes; read-modify-write 4 bytes | Load and store 1 to 8 bytes; read-modify-write 4 and 8 bytes | Load and store of 1 byte |
| `_Thread_local` | Per thread | No | No | Per thread | No | One shared instance | One shared instance | One shared instance | One shared instance |

"One shared instance" means the object is placed in `.tbss` but addressed
as an ordinary static object; see [Targets](targets.md#summary).

The errors for the "No" entries:

| Case | Diagnostic |
|---|---|
| `long double` or `_Float128` arithmetic on RISC-V, `__int128` arithmetic on RV64 | `the RV64 backend cannot lower a 128-bit value yet (function f) [...]` (`RV32` on RV32) |
| `long double` in a signature on Windows | `long double in the signature of 'f' is not supported for a Windows target yet: there it travels by reference and returns through a hidden pointer, and EmbCC passes it on the stack by value` |
| `long double _Complex` arithmetic on Windows | `passing long double is not supported for a Windows target yet: there it travels by reference and returns through a hidden pointer, and EmbCC passes it on the stack by value` |
| `__int128` in a signature on Windows | `__int128 in the signature of 'f' is not supported for a Windows target yet: EmbCC lowers its arithmetic to libgcc helpers whose arguments it places in the System V registers` |
| `__int128` on a 32-bit target or AVR | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| `_Float128` on x86-64 | `` _Float128 is not supported on x86-64: `long double` here is x87's 80-bit extended format, not IEEE binary128, so it is not the same type `` |
| `_Float128` where `long double` is not 16 bytes | `_Float128 is not supported on this target: it has no 128-bit floating-point type` |
| An `_Atomic` object wider than the machine moves at once | `an atomic access of 8 bytes is not one access on this target (it moves 4 at once): the halves could be split by an interrupt or another core` |
| `_Thread_local` on macOS | `__thread is not supported for a Darwin target yet: Mach-O addresses a thread-local through a __thread_vars descriptor, which this writer does not emit` |
| `_Thread_local` on Windows | `__thread is not supported for a Windows target yet: Windows reaches a thread-local through a _tls_index and a TLS directory this writer does not emit` |
| `va_arg` of a structure on Windows | `va_arg of a struct is not supported for a Windows target yet` |

The text in square brackets names the internal operation that could not
be lowered.

## Feature notes

### Functions that can reach their closing brace

A function with a non-`void` return type, other than `main`, must end
every path with a `return` statement. The standard allows control to
reach the closing brace as long as the caller does not use the value;
EmbCC refuses it:

```text
error: control may reach the end of 'f' — every path must end in a return statement [E0008]
```

`main` is the exception the standard makes: reaching its closing brace
returns 0.

A path also ends at an infinite loop, at a call to a function declared
`_Noreturn`, `[[noreturn]]` or `__attribute__((noreturn))`, and at a call
to `exit`, `_Exit`, `abort`, `__builtin_unreachable` or
`__builtin_trap`. `embcc --explain E0008` describes the rule; see
[Diagnostics](diagnostics.md#t6).

### Empty parameter lists

A function declarator with an empty parameter list, `int f();`, declares
a function with no parameters, as in C23, whatever `-std=` says. Code
that relies on the older meaning, "parameters not specified", is refused:

| Code | Diagnostic |
|---|---|
| `int f(); ... f(1);` | `this call needs 0 arguments, got 1` |
| `int f(); int f(int a) { ... }` | `conflicting declaration of 'f'` |
| `int (*fp)() = add;` where `add` takes parameters | `initialization: cannot convert function * to function * without a cast` |

### Scope of tags, typedefs and enumeration constants

Objects, functions and labels have the scopes the standard gives them. A
struct, union or enum tag, a typedef name and an enumeration constant
are different: there is one namespace for each kind per translation
unit, and a declaration in a block stays visible after the block ends.
This has three consequences.

A name declared in a block can be used outside it, which the standard
does not allow:

```c
int f(void) { typedef int T; struct S { int a; } s = { 1 }; return s.a; }
int g(void) { T y = 2; struct S z = { y }; return z.a; }   /* accepted */
```

The same name declared with a different meaning in two blocks, or in a
block and at file scope, is refused although the standard allows it:

| Declarations | Diagnostic |
|---|---|
| `struct S { int a; };` at file scope, `struct S { double d; } x;` in a function | `redefinition of 'S'` |
| `typedef int T;` in one function, `typedef double T;` in another | `redefinition of typedef 'T'` |
| `enum { A = 1 }` in one function, `enum { A = 2 }` in another | `duplicate enumerator 'A'` |
| `enum { A = 1 };` at file scope, `constexpr int A = 2;` in a function | `'A' is already a named constant (line 1); ...` |
| `int N;` in a function, then `enum { N = 3 };` in a block inside it | `'N' would hide the local 'N' declared before it in this function; ...` |
| `enum { N = 3 };` in a function, and a variable or function `N` at file scope | `'N', declared in a function body, has the name of the file-scope variable on line 2; ...` |

The last two would otherwise give the name two meanings in one program
-- the constant in one place, the variable in another -- so they are
refused rather than resolved either way. At file scope, an enumeration
constant and a variable or function of the same name are the
standard's redeclaration error:

```text
error: 'g' redeclared as a different kind of symbol: it is also the function on line 2
```

A tagged structure or union, or any enumeration, cannot be defined
inside a type name in an expression (a cast, `sizeof`, a compound
literal):

```text
error: define structs/unions at file scope (block-scope type definitions are not supported)
error: define enums at file scope (block-scope type definitions are not supported)
```

An untagged structure in a cast, `(struct { int a; } *)p`, is accepted.
A local variable or a parameter may share its name with a typedef name
or with an enumeration constant declared before it, and hides it, as the
standard specifies. That includes an array bound:

```c
enum { N = 3 };
int f(void) { int N = 5; int a[N]; return sizeof a; }   /* 20, a VLA of five */
```

### Const qualification

`const` is part of a type, at every level: `const char *` and
`char *const` are different types from `char *` and from each other.
An lvalue whose type is `const`-qualified, or a structure or union with
a `const` member at any depth, cannot be modified. Each of these is an
error, as the standard requires:

```c
const int limit = 10;
void f(void) { limit = 11; }        /* assignment of read-only 'limit' */
void g(const int *p) { *p = 3; }    /* assignment of a read-only location */
void h(void) { limit++; }           /* increment of read-only 'limit' */
```

```text
error: assignment of read-only 'limit' (its type is const int)
error: assignment of a read-only location (its type is const int)
error: assignment of struct S, which has a const member
```

A member of a `const` structure is `const`, and so is an element of a
`const` array. Initialization is not assignment: a `const` object takes
its value from its initializer.

Converting a pointer to a `const`-qualified type into a pointer whose
pointed-to type is not `const`, without a cast, is diagnosed with the
warning `-Wdiscarded-qualifiers`, which is on by default as in GCC:

```text
warning: initialization discards the 'const' qualifier of const char * [-Wdiscarded-qualifiers]
```

A cast removes `const` without a diagnostic. A `const` object with
static storage duration is placed in a read-only section (`.rodata`).
`__auto_type` and the value of an expression drop the qualifiers;
`typeof` keeps them, and `typeof_unqual` drops them.

### Enumeration constants

An enumeration with a fixed underlying type, `enum E : unsigned char`,
has that type, and so do its constants. A value that the underlying type
cannot represent is refused:

```text
error: enumerator 'B' is 256, which the underlying type unsigned char cannot represent
```

An enumeration without a fixed underlying type is `int` (two bytes on
AVR) while every value fits `int`, and its constants then have type
`int`. A constant outside the range of `int`, which C17 does not allow
and C23 does, is accepted without a diagnostic. The enumeration then
takes the first of these types that can represent every value, and, as
C23 specifies, its constants take that type too:

1. `unsigned int`, `unsigned long`, then `unsigned long long`, if no
   value is negative;
2. `long`, then `long long`, otherwise.

```c
enum big { HUGE = 0x100000005 };      /* unsigned long on x86-64: sizeof(enum big) is 8 */
long long f(void) { return HUGE; }    /* returns 0x100000005 */
```

Where this choice differs from GCC's and Clang's is described in
[Implementation-defined behavior](implementation-defined.md#structures-unions-enumerations-and-bit-fields).
`-fshort-enums` is refused (see [Targets](targets.md#data-models)).

A value of 2^63 or more, from an `unsigned long long` initializer, is
that value: `enum { X = 0xffffffffffffffff }` is an `unsigned long`
enumeration on the 64-bit targets, and `X > 0`. Two enumerations have no
type that can represent every value, and are refused:

```text
error: the enumeration's values run from -1 to 9223372036854775808, which no integer type holds
error: enumerator 'B' would be one past LLONG_MAX, which no integer type the enum can have holds
```

The first is a negative value together with one of 2^63 or more. The
second is a constant without an initializer that follows `LLONG_MAX`
(or, with `ULLONG_MAX` in the message, follows `ULLONG_MAX`).

### Inline functions

EmbCC follows C11 6.7.4p7. When every file-scope declaration of a
function with external linkage in a translation unit says `inline` and
none says `extern`, its definition there is an *inline definition*:

- It provides no external definition. No symbol is defined for it in
  the object.
- The optimizer may inline its calls. A call that is not inlined, every
  call at `-O0`, and any use of its address refer to the external
  definition, which is an undefined symbol in this object.
- Exactly one translation unit of the program must provide the external
  definition. The usual way is a declaration with `extern`, or without
  `inline`, in one `.c` file that includes the header:

```c
/* twice.h */
inline int twice(int x) { return 2 * x; }

/* twice.c -- the one external definition */
#include "twice.h"
extern inline int twice(int);
```

A `static inline` function is unaffected: it has internal linkage, and
each unit that uses it has its own copy. A function that is `main`, a
constructor or destructor, `__attribute__((used))`, or the target of an
`alias` is always emitted.

GNU89 semantics apply instead with `-fgnu89-inline`, with `-std=c89`,
`-std=c90`, `-std=iso9899:1990`, `-std=gnu89` or `-std=gnu90`, and for
a function with `__attribute__((gnu_inline))`. Under them a definition
that says `inline` alone is an external definition, and one that says
`extern inline` is used only for inlining and never emitted. See
[`-fgnu89-inline`](invoking.md#-fgnu89-inline).

Whether a call is inlined is decided by the optimizer; see
[Optimization](optimization.md).

### Variable length arrays

Supported: VLAs of any number of dimensions at block scope, VLA
parameters, pointers to VLAs and `[*]` in prototypes. The size
expression is evaluated once, where the declaration is reached; `sizeof`
of a VLA is computed at run time; the storage is released when control
leaves the block, including by `break`, `continue` and `goto`. This
includes a typedef of a variably modified type, whose size is fixed
where the typedef is declared: after
`int n = 3; typedef int row[n]; n = 10;`, `sizeof(row)` is 12.

Refused:

| Declaration | Diagnostic |
|---|---|
| A VLA at file scope or as a structure member | `array size must be a constant expression here (a variable length array can only be a local variable or a parameter)` |
| `static int a[n];` | `static 'a' cannot have a variably modified type (int[*])` |
| `int a[n] = { 1 };`, `int a[n] = {};` | `variable length array 'a' cannot be initialized` |

A `goto` or `switch` that jumps into the scope of a VLA is not
diagnosed, although the standard requires a diagnostic.

### Complex types

`float _Complex`, `double _Complex` and `long double _Complex` are
supported, with all arithmetic operators. A bare `_Complex` is
`double _Complex`. `<complex.h>` defines `complex`, `I` and `_Complex_I`
and declares the library functions. `long double _Complex` depends on
the target; see [Target-dependent features](#target-dependent-features).

The GNU integer complex types are refused:

```text
error: invalid _Complex type (only float, double and long double _Complex are supported)
```

A complex object with static storage duration takes an initializer that
EmbCC can fold. Otherwise:

```text
error: a static complex initializer must be a constant that EmbCC folds: literals, casts, + and -, and scaling by a real
```

This rules out a product or quotient of two complex values, and
`CMPLX(1.0, 2.0)`, which `<complex.h>` defines as a call to a library
function. At block scope both are accepted.

### Generic selection

`_Generic` selects the association whose type is the type of the
controlling expression after lvalue conversion, which removes the
expression's own qualifiers and `_Atomic`. Types are compared exactly:
`long` and `long long` are different types even on targets where they
have the same size, plain `char` is neither `signed char` nor
`unsigned char`, and `const`, `volatile` and `_Atomic` in a pointed-to
type count:

```c
_Generic((const char *)0, char *: 1, default: 2)   /* 2 */
_Generic(1, const int: 1, default: 2)              /* 2: the operand is an int */
_Generic(1L, long long: 1, long: 2)                /* 2 */
```

A selection in which two associations have the same type is refused, as
the standard requires:

```text
error: more than one _Generic association matches type int: two associations name the same type
```

### Atomic types

The operators on an `_Atomic` object (load, store, `++`, `--` and the
compound assignments) are atomic for integer and pointer types. Other
types are refused, with one of:

```text
error: reading an _Atomic double is not supported: EmbCC makes the operators atomic for integers and pointers only
error: assigning to an _Atomic struct s is not supported: EmbCC makes the operators atomic for integers and pointers only
error: a compound assignment to an _Atomic double is not supported: EmbCC makes the operators atomic for integers and pointers only
```

The sizes each target supports are in
[Target-dependent features](#target-dependent-features). On x86-64 and
AArch64 the operators handle up to 8 bytes; a 16-byte `_Atomic __int128`
is refused with `an atomic access of 16 bytes is not one access on this
target (it moves 8 at once): ...`, while the `<stdatomic.h>` functions
and the `__atomic_*` builtins handle 16 bytes on those targets.

`<stdatomic.h>` is the C library's interface over the compiler's atomic
builtins; see [Libraries](libraries.md) and [Extensions](extensions.md).

### Alignment specifiers

`_Alignas(N)`, `_Alignas(type)` and `_Alignof(type)` are supported on
objects and structure members, in any order among the declaration
specifiers. An automatic array, structure or union may have an alignment
larger than the stack's own (16 bytes; 8 on Cortex-M), and gets it. The
`alignas` and `alignof` spellings are keywords (see
[Keywords](#keywords)). The cases the standard constrains, and the ones
EmbCC limits, are handled as follows:

| Case | Behavior |
|---|---|
| `_Alignas(0)` | Accepted, with no effect, as the standard specifies. |
| An alignment that is not a power of two, `_Alignas(3)` | Refused: `_Alignas requires a constant power of two`. |
| An alignment weaker than the type's, `_Alignas(1) int` | Not diagnosed. The type's own alignment is kept. |
| `_Alignas` in a typedef or on a parameter | Not diagnosed. |
| An automatic object aligned beyond the stack's alignment, `_Alignas(32) int x;` | Supported, scalar or aggregate: its storage is carved from the stack at function entry and rounded up to the alignment, so its address has it at any call depth. A scalar so aligned lives in that storage rather than in a register. |

`_Alignof` applied to an expression is a GNU extension; see
[Extensions](extensions.md).

### Initializers

- **Empty initializer.** `= {}` zero-initializes the whole object: a
  scalar, an array, a structure or a union of any storage duration, a
  nested aggregate (`{ {}, 1 }`) and a compound literal (`(struct p){}`).
  A VLA cannot be initialized; see
  [Variable length arrays](#variable-length-arrays).
- **Flexible array members.** See the [C99](#c99) table.
- **File-scope compound literals.** See the [C99](#c99) table.
- **Brace elision in an array of unknown size.** When an initializer
  that omits inner braces could give the array a different number of
  elements depending on the types of its expressions, the declaration is
  refused rather than sized one way:

  ```text
  error: cannot size 'g' from its initializer: the braces it leaves out read as 2 elements without the types and 1 with them; brace each element
  ```

  This happens, for example, with `struct pt { int x, y; }; static struct pt g[] = { 1 ? 2 : 3, 4 };`.
  Bracing each element, `{ { 1 ? 2 : 3, 4 } }`, is accepted.
- **Braces around a scalar.** One level is accepted, `int q = { 4 };`.
  More is refused: `braces around a scalar initializer take one level`.

### Constexpr objects

`constexpr` declares a named integer constant, at file scope or in a
block, usable wherever an integer constant expression is:

```c
constexpr int N = 4;
constexpr long long BIG = 5000000000;
int table[N];
```

| Case | Diagnostic |
|---|---|
| A non-integer type (floating, pointer, structure) | `constexpr 'd' of type double is not supported: EmbCC takes integer constants` |
| A value the type cannot represent exactly | `constexpr 'c': 300 does not fit unsigned char, and C23 wants it exactly` |
| A non-constant initializer | `constexpr 'k' needs a constant initializer; this one is not one` |
| No initializer | `constexpr 'k' needs an initializer` |
| Assignment to the object | `assignment target is not an lvalue` |
| Taking its address, `&k` | `'&' needs a variable or *pointer` |

A `constexpr` name has no storage, which is why its address cannot be
taken. `typeof` and `auto` cannot see its type (see
[Type inference](#type-inference)).

### Type inference

`typeof(...)` and `typeof_unqual(...)` accept a type name or an
expression. `auto` (C23) and `__auto_type` (GNU) declare an object whose
type is that of its initializer. All three determine an expression's type
before the rest of the semantic analysis, and accept these expression
forms: identifiers of objects (including parameters and function
pointers), integer, floating, character and narrow string constants,
`nullptr`, casts, calls, subscripts, `*`, `&` of an object, `.` and `->`,
unary `-`, `!` and `~`, the binary arithmetic, bitwise, shift,
comparison and logical operators, pointer arithmetic and `?:`. The type follows the
usual conversions: `typeof(c + c)` with `char c` is `int`, and
`typeof(arr)` keeps the array type.

Other forms are refused: compound literals, `sizeof` and `_Alignof`,
`++` and `--`, assignments, the comma operator, a function name, wide
string literals, `__real__` and `__imag__`, `_Generic`, statement
expressions, and enumeration and `constexpr` constants (including
`true` and `false`). The diagnostic is

```text
error: typeof of an unsupported expression
```

for `typeof`, and for `auto` and `__auto_type`

```text
error: __auto_type cannot see the type of this initializer
```

An assignment or comma expression inside `typeof` is a syntax error,
for example `expected ')' after typeof before ','`.

`auto` infers a type only at block scope, for one declarator, with an
initializer and no other specifiers before it except `static`:

| Declaration | Diagnostic |
|---|---|
| `auto g = 3L;` at file scope | `expected a type before 'auto'` |
| `auto x;`, `const auto x = 1;` | `expected a type in this declaration, found 'auto'` |
| `auto a = 1, b = 2;` | `expected ';' before ',' [E0002]` |

`auto int x;` is the C89 storage class and is accepted.

### Attributes

The `[[...]]` syntax shares its implementation with
`__attribute__((...))`: `[[gnu::NAME]]` is the GNU attribute `NAME`, and
the standard attributes map onto GNU ones (`maybe_unused` to `unused`,
`nodiscard` to `warn_unused_result`, `_Noreturn` to `noreturn`). Any
attribute namespace is dropped, so `[[gnu::packed]]` and `[[packed]]`
are the same attribute. The GNU attributes and how each is handled are
listed in [Extensions](extensions.md).

`[[...]]` is accepted in these positions:

- at the start of a declaration, at file scope and in a block;
- after `struct`, `union` or `enum`: `struct [[gnu::packed]] S { ... };`;
- before a parameter declaration: `int f([[maybe_unused]] int a)`;
- after a function declarator: `void f(void) [[gnu::noinline]];`;
- before a statement, including the empty attribute statement
  `[[fallthrough]];` and an empty list `[[]]`.

It is refused after the declarator of an object, and after an array
declarator, with `expected an expression, got '['`, and on an
enumerator with `expected '}' before '['`. An attribute at the start of a
declaration that declares no object or function,
`[[gnu::packed]] struct S { ... };`, applies to nothing and is ignored
without a diagnostic; put a type attribute after the `struct` keyword.

An attribute EmbCC does not know is ignored with a `-Wattributes`
warning, as for the GNU syntax.

### Embedding binary data

`#embed "file"` and `#embed <file>` expand to the bytes of the file as a
comma-separated list of integer constants, for use in an initializer:

```c
static const unsigned char logo[] = {
#embed "logo.bin"
};
```

The file is searched for as `#include` searches for a header: a quoted
name first in the directory of the file that contains the directive,
then in the include directories; an angle-bracketed name in the include
directories only. An empty file expands to nothing.

| Case | Diagnostic |
|---|---|
| Any parameter (`limit`, `prefix`, `suffix`, `if_empty`, or a vendor one) | `#embed parameters (limit, prefix, suffix, if_empty) are not supported; ignoring one would embed the wrong bytes` |
| A file name given by a macro, `#embed FILE` | `#embed needs a file name` |
| A file that is not found | `cannot find the file to embed: "logo.bin"` |

`__has_embed` is not supported.

### Headers

The headers EmbCC ships are described in [Libraries](libraries.md). The
language-related declarations they do not provide:

| Missing | Standard | Header |
|---|---|---|
| `nullptr_t`, `unreachable()` | C23 | `<stddef.h>` |
| `char8_t` | C23 | `<uchar.h>` |
| `INT_WIDTH`, `BOOL_WIDTH`, `LLONG_WIDTH`, ... | C23 | `<limits.h>` |
| `INT8_WIDTH`, `SIZE_WIDTH`, ... | C23 | `<stdint.h>` |
| `FLT_NORM_MAX`, `DBL_NORM_MAX`, ... | C23 | `<float.h>` |

`CMPLX`, `CMPLXF` and `CMPLXL` are defined as function calls; see
[Complex types](#complex-types).

## Preprocessor

The preprocessor is integrated in `embcc`; `-E` writes its output. The
command-line options that control it (`-D`, `-U`, `-I`, `-isystem`, the
dependency options) are in
[Invoking EmbCC](invoking.md#preprocessor-options). The GNU directives and
operators (`#include_next`, `__has_include_next`, `__has_builtin`,
`__has_attribute`, `#pragma pack`, ...) are in [Extensions](extensions.md).

### Translation phases

| Phase | Status | Notes |
|---|---|---|
| Source character set | Supported | Source files are read as UTF-8. A leading byte-order mark is skipped. |
| Trigraphs | Not supported | Never replaced. |
| Line splicing | Partial | A backslash-newline is removed between tokens, in directives and inside identifiers and numbers. Inside a string literal or character constant it is an error (shown below the table). At the end of a `//` comment it does not continue the comment: the next line is compiled as code. |
| Comments | Supported | Each comment is replaced by one space. |
| Digraphs | Not supported | See [C95](#c95-amendment-1). |
| Universal character names | Partial | In character constants and string literals only; see the [C99](#c99) table. |
| String literal concatenation | Supported | |

A backslash-newline inside a string literal or character constant gives:

```text
error: unknown escape '\
' in a literal
```

### Directives

| Directive | Status | Notes |
|---|---|---|
| `#include "file"`, `#include <file>` | Supported | The search order is in [Invoking EmbCC](invoking.md#search-order). |
| `#include` with a macro-expanded name, `#include HEADER` | Not supported | `malformed #include` |
| `#define`, `#undef` | Supported | Redefining a macro with a body that differs only in white space is accepted with a warning, `macro 'NAME' redefined`; the standard allows it silently. |
| `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif` | Partial | See [Conditional expressions](#conditional-expressions). |
| `#elifdef`, `#elifndef` | Supported | |
| `#line DIGITS ["file"]` | Supported | A missing or zero line number: `#line needs a positive line number`. |
| `#error TEXT` | Supported | An error: `#error: TEXT`. |
| `#warning TEXT` | Supported | A warning: `#warning: TEXT`. |
| `#pragma` | Partial | `pack`, `once`, `push_macro`, `pop_macro` and `weak` are acted on (see [Extensions](extensions.md#pragmas)). Every other pragma, including the `STDC` pragmas, is ignored without a diagnostic. |
| `_Pragma("...")` | Supported | The same as the corresponding `#pragma`. |
| `#embed` | Partial | See [Embedding binary data](#embedding-binary-data). |
| `#` alone (null directive) | Supported | |
| Any other directive | | In a group that is being compiled, an error: `unknown directive '#ident'`. In a group skipped by a conditional, ignored. |

### Conditional expressions

`#if` and `#elif` support `defined`, the unary operators `+ - ~ !`, the
binary arithmetic, shift, relational, equality, bitwise and logical
operators, and parentheses. Identifiers that are not macros evaluate to
0, except `true`, which evaluates to 1. Integer constants may be
decimal, octal, hexadecimal or binary, with digit separators; character
constants have the value they have in code. Differences from the
standard:

| Case | Behavior |
|---|---|
| The conditional operator, `#if A ? B : C` | Not supported: `trailing junk in #if expression` |
| Unsigned arithmetic | Every value is a signed 64-bit integer; a `u` suffix is ignored. `#if -1 < 0u` is true; the standard makes it false. |
| Division or remainder by zero | An error even in an operand that `&&`, `\|\|` does not evaluate: `#if 0 && (1/0)` gives `division by zero in #if`. |
| A multi-character constant | `bad character constant in #if` |

### Macro expansion

Object-like and function-like macros, the `#` and `##` operators,
variadic macros with `__VA_ARGS__`, `__VA_OPT__`, empty arguments and
rescanning are supported. Stringizing (`#`) differs from the standard in
its handling of white space:

| Macro use | EmbCC | Standard |
|---|---|---|
| `#define S(x) #x` then `S(a   b)` | `"a   b"` | `"a b"` |
| `#define V(...) #__VA_ARGS__` then `V(a, b)` | `"a,b"` | `"a, b"` |

Runs of white space inside an argument, including a line break, are kept
as written instead of becoming one space, and the space after each comma
between variadic arguments is dropped.

### Feature-test operators

| Operator | Status | Notes |
|---|---|---|
| `defined NAME`, `defined(NAME)` | Supported | |
| `__has_include(<file>)`, `__has_include("file")` | Supported | `#ifdef __has_include` is true. |
| `__has_c_attribute(NAME)` | Partial | Defined (`#ifdef __has_c_attribute` is true), and 0 for every name. |
| `__has_embed(...)` | Not supported | Not defined; see the [C23](#c23) table. |
