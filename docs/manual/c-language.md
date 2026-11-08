# C Language Support

This page describes the C language EmbCC accepts: the `--std=` values and
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

- C23 (ISO/IEC 9899:2024), with the C17 behaviour where C23 changed
  the meaning of a construct and `--std=` names an earlier standard;
- several features of the C2y working draft, listed under
  [Later drafts](#later-drafts);
- the GNU extensions described in [Extensions](extensions.md).

`--std=` selects `__STDC_VERSION__` and the few rules that differ between
the standards: `202311L` under `--std=c23` or `gnu23`, and `201710L`
under the default, `gnu17`.

As a rule, a construct EmbCC does not implement is refused with an error
that names it. The exceptions are the places where EmbCC accepts a
program the standard requires it to diagnose, or gives a program a
meaning other than the standard's. Check these first when porting code:

- Struct, union and enum tags, typedef names and enumeration constants
  declared in a block remain visible to the end of the translation unit.
  See [Scope of tags, typedefs and enumeration constants](#scope-of-tags-typedefs-and-enumeration-constants).

EmbCC is also stricter than the standard in one place that commonly
affects existing code: an empty parameter list `()` always means "no
parameters" ([details](#empty-parameter-lists)). A non-`void` function
that can reach its closing brace gets a warning that is on by default
([details](#functions-that-can-reach-their-closing-brace)).

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

### `--std=STANDARD`

GCC: `-std=STANDARD`.

Name a C standard. Each is compiled as it is: its keywords, its
`__STDC_VERSION__`, C89's implicit `int` and C99's `//` comments, the
unprototyped `int f()` before C23, and trigraphs under a strict standard
before C23. The full rules are under
[`--std=`](invoking.md#--stdstandard).

| `--std=` value | Standard | `__STDC_VERSION__` |
|---|---|---|
| `c89`, `c90`, `iso9899:1990`, `gnu89`, `gnu90` | C89/C90 | not defined |
| `iso9899:199409` | C94 | `199409L` |
| `c99`, `c9x`, `iso9899:1999`, `gnu99`, `gnu9x` | C99 | `199901L` |
| `c11`, `c1x`, `iso9899:2011`, `gnu11`, `gnu1x` | C11 | `201112L` |
| `c17`, `c18`, `iso9899:2017`, `iso9899:2018`, `gnu17`, `gnu18` | C17 | `201710L` |
| `c23`, `c2x`, `iso9899:2024`, `gnu23`, `gnu2x` | C23 | `202311L` |
| `c2y`, `gnu2y` | C2y | `202500L` |
| any other value | | Error |

The default is `gnu17`. A `cNN` value defines `__STRICT_ANSI__`. `-ansi`
is `--std=c89`. Any other value is an error:

```text
embcc: error: unknown standard '-std=c3x'
```

[`--pedantic` and `--pedantic-errors`](invoking.md#--pedantic---pedantic-errors)
report, as a warning or an error, what the standard does not have: a
later standard's feature, or a GNU extension.

See [Diagnostics](diagnostics.md).

The C++ values (`c++20`, `gnu++17`, ...) are described in
[Invoking EmbCC](invoking.md#--stdstandard) and [C++ support](cxx.md).

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

- `#if __STDC_VERSION__ >= 202311L` is false by default, although many
  C23 features are available; it is true under `--std=c23`. Test for the
  feature, or for `__EMBCC__`.
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
| `__STDC_VERSION__` | The standard `--std=` names: `202311L` for C23, `201710L` for C17 and the default `gnu17`, `201112L` for C11, `199901L` for C99, `199409L` for C95 and `202500L` for C2y; not defined for C89 |
| `__STDC_HOSTED__` | `1`; `0` under `--freestanding` (`-ffreestanding`) |
| `__FILE__`, `__LINE__` | Supported; `#line` changes both |
| `__DATE__`, `__TIME__` | Supported; `SOURCE_DATE_EPOCH` sets them, as in GCC. See [Implementation-defined behavior](implementation-defined.md#preprocessing-directives). |
| `__STDC_IEC_559__`, `__STDC_IEC_559_COMPLEX__` | `1` on every target but AVR, whose `double` is 32 bits. Under `--std=c23`, also `__STDC_IEC_60559_BFP__` and `__STDC_IEC_60559_COMPLEX__`, `202311L` |
| `__STDC_UTF_16__`, `__STDC_UTF_32__` | `1`: `char16_t` and `char32_t` hold UTF-16 and UTF-32 |
| `__STDC_ISO_10646__`, `__STDC_MB_MIGHT_NEQ_WC__` | Not defined: they describe the C library's `wchar_t`, and a library that has them defines them |
| `__STDC_ANALYZABLE__`, `__STDC_LIB_EXT1__` | Not defined |
| `__STDC_NO_ATOMICS__`, `__STDC_NO_COMPLEX__`, `__STDC_NO_THREADS__`, `__STDC_NO_VLA__` | Not defined |

## C89 and C90

| Feature | Status | Notes |
|---|---|---|
| Declarations, expressions and statements | Supported | Except as listed in this table and under [Feature notes](#feature-notes). |
| Function prototypes | Supported | |
| Empty parameter list `int f();` | Supported | Before C23, a declaration with unspecified parameters; under `--std=c23`, "no parameters". See [Empty parameter lists](#empty-parameter-lists). |
| Old-style (K&R) function definitions | Supported | `int add(a, b) int a, b; { ... }`. The parameters are promoted as the standard says. C23 removed them; EmbCC still accepts them there, as GCC does. |
| Implicit `int` | Partial | Under `--permissive` (GCC's `-fpermissive`), `static x = 3;` declares an `int`, with a warning: `type defaults to 'int' in declaration of 'x' [--warn=implicit-int]`. Without it, as in GCC 14: `expected a type before 'x'`. C99 removed it. |
| Implicit function declarations | Partial | Under `--permissive`, a call of an undeclared function declares it `int f()`, with a warning: `implicit declaration of function 'foo' [--warn=implicit-function-declaration]`. Without it: `'foo' is not declared in 'main' — for a call, add a prototype or define it first [E0001]`. C99 removed them. |
| Reaching the `}` of a non-`void` function | Supported | A warning, on by default, except in `main`: `control may reach the end of 'f', which returns int [--warn=return-type] [E0008]`. See [Functions that can reach their closing brace](#functions-that-can-reach-their-closing-brace). |
| `return;` in a non-`void` function | Supported | The caller must not use the value. From C99 on, where it is a constraint violation: `'g' returns int; 'return' needs a value`. |
| `return x;` in a `void` function | Supported | `x` is evaluated and dropped, with a warning that is on by default: `returning a value from void 'f' [--warn=return-mismatch]` (an error under `--pedantic-errors`, and from C99 on). A `void` `x`, `return f();`, is a GNU C form: see [Smaller GNU C forms](extensions.md#smaller-gnu-c-forms). |
| `auto` storage class | Supported | `auto int x;` |
| `register` storage class | Partial | Accepted on block-scope objects and on parameters, where it is a hint the allocator does not need. At file scope, which C does not allow: `'register' is not allowed here: C allows it on a block-scope object and on a parameter (C11 6.9p2 for file scope)`. Taking the address of a `register` object is not diagnosed. Any other storage class on a parameter: `a parameter can have no storage class but 'register'`. |
| Storage class after the type, `const static int t[]`, `int static n`, `long typedef L` | Partial | Obsolescent (C17 6.11.5), and `-Wold-style-declaration` (`-Wextra`) reports it, as in GCC. At block scope `extern` and `typedef` must come first: `at block scope, write 'extern' first in the declaration`. |
| `const` | Supported | See [Const qualification](#const-qualification). |
| `volatile` | Supported | |
| Scopes | Partial | Objects, functions and labels follow the standard's rules. Tags, typedef names and enumeration constants do not. See [Scope of tags, typedefs and enumeration constants](#scope-of-tags-typedefs-and-enumeration-constants). |
| Structures, unions, bit-fields | Supported | A structure or union with no members is a GNU extension: size 0, as in GCC, and an error under `--pedantic-errors`. |
| Enumerations | Supported | A constant outside the range of `int` is accepted without a diagnostic and gives the enumeration a wider type, as in C23. See [Enumeration constants](#enumeration-constants). |
| Tentative definitions | Supported | A file-scope array of unknown size, `int a[];`, takes its size from a later declaration (`int a[10];`); if none gives one, it has one element, with a warning: `array 'a' assumed to have one element` (C17 6.9.2p5). |
| `goto`, labels, `switch` | Supported | |
| Adjacent string literal concatenation | Supported | A prefixed and an unprefixed literal concatenate: `L"wi" "de"` is `L"wide"`. |
| Wide literals `L"..."`, `L'x'` | Supported | |
| Multi-character constants `'ab'` | Supported | GCC's value, reported by `-Wmultichar` as in GCC. See [Implementation-defined behaviour](implementation-defined.md). A wide one, `L'ab'`, is refused: `a wide character constant holds one character`. |
| Trigraphs `??=` `??(` ... | Not supported | Not replaced: `"??="` is a three-character string. C23 removed them. |

### C95 (Amendment 1)

| Feature | Status | Notes |
|---|---|---|
| Digraphs `<:` `:>` `<%` `%>` `%:` `%:%:` | Supported | The same tokens as `[` `]` `{` `}` `#` `##`, in directives and in macros; `#` of a digraph spells it as written. |
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
| Variable length arrays | Supported | On every target, AVR included. See [Variable length arrays](#variable-length-arrays). |
| Variably modified types: pointers to VLAs, VLA parameters `int a[n][m]`, `[*]` | Supported | A definition evaluates its parameters' lengths on entry, the outermost too, though the parameter is a pointer: `int a[i++]` increments `i`. A length is an assignment-expression, so `int a[i += 2]` is one. |
| `static` and qualifiers in array parameters, `int a[static 4]` | Supported | The qualifiers apply to the pointer the parameter becomes: `int a[const 4]` is `int *const a`. `static` promises at least that many elements and changes no code. Outside a parameter: `expected an expression, got 'static'`. |
| Flexible array members | Supported | An initializer for the flexible member is accepted only on a static object, which is a GNU extension. On an automatic object: `'l' is not static, so its flexible array member 'd' cannot be initialized: the object has no room for the elements`. |
| Designated initializers | Supported | |
| Compound literals | Supported | At file scope and in a block, `int *p = (int[]){ 1, 2, 3 };` included. Storage-class specifiers in a compound literal are C23; see the [C23](#c23) table. |
| Non-constant initializers for automatic aggregates | Supported | |
| `_Complex` and `<complex.h>` | Partial | `float`, `double` and `long double` complex types. Static initializers are limited. See [Complex types](#complex-types). |
| `_Imaginary` | Not supported | Not a keyword; a declaration that uses it is a syntax error. |
| Hexadecimal floating constants `0x1.8p1` | Supported | |
| Universal character names | Supported | In character constants, string literals and identifiers: `int caf\u00e9;` is the identifier `café`. |
| `__func__` | Supported | |
| Variadic macros, `__VA_ARGS__` | Supported | |
| Empty macro arguments | Supported | |
| `_Pragma` operator | Supported | |
| `#pragma STDC FP_CONTRACT`, `FENV_ACCESS`, `CX_LIMITED_RANGE` | Partial | Accepted and ignored. |
| `va_copy` | Supported | |
| Trailing comma in an enumerator list | Supported | |
| `main` returns 0 when it reaches `}` | Supported | |
| Integer division truncates toward zero | Supported | |
| `__STDC_IEC_559__` and the other conditional feature macros | Supported | `__STDC_IEC_559__` (and C23's `__STDC_IEC_60559_BFP__`) where `float` and `double` are IEEE binary32 and binary64, which is every target but AVR, whose `double` is 32 bits; `__STDC_IEC_559_COMPLEX__` with them. See [Predefined macros](#predefined-macros). |

## C11

| Feature | Status | Notes |
|---|---|---|
| `_Alignas`, `_Alignof`, `<stdalign.h>` | Partial | See [Alignment specifiers](#alignment-specifiers). |
| `_Noreturn`, `<stdnoreturn.h>` | Supported | The same as `__attribute__((noreturn))`. |
| `_Static_assert` | Supported | A failed assertion: `static assertion failed: MESSAGE`. A non-constant condition: `_Static_assert needs a constant integer expression`. |
| `_Generic` | Supported | See [Generic selection](#generic-selection). With no matching association and no `default`: `no _Generic association matches type double`. |
| `_Atomic` qualifier and `_Atomic(T)` specifier | Partial | Integer and pointer types only; sizes depend on the target. See [Atomic types](#atomic-types). |
| `<stdatomic.h>` | Supported | See [Atomic types](#atomic-types). |
| `_Thread_local` | Partial | Per-thread storage on the x86-64 and AArch64 ELF, EmbLinkOS, Linux, macOS and Windows (emulated TLS, as MinGW's) targets; one shared instance on Cortex-M, RISC-V, MIPS32 and AVR. See [Target-dependent features](#target-dependent-features). At block scope without `static` or `extern`: `a block-scope __thread object must also be static: an automatic one is already private to the call`. |
| Anonymous structures and unions | Supported | |
| `char16_t`, `char32_t`, `u"..."`, `U"..."`, `u'x'`, `U'x'`, `u8"..."` | Supported | `<uchar.h>` is provided by the C library. |
| `max_align_t` | Supported | Aligned as `long double` or `long long`, whichever is stricter: 16 bytes on x86-64, AArch64 and RISC-V, 8 on Cortex-M and MIPS32, 1 on AVR. |
| `CMPLX`, `CMPLXF`, `CMPLXL` | Supported | Constant expressions when their operands are, so they initialize a static object. See [Complex types](#complex-types). |
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
| `nullptr`, `nullptr_t` | Supported | `nullptr` has type `nullptr_t`, which `<stddef.h>` declares. It converts to every pointer type and to `bool`. |
| `constexpr` objects | Supported | Of every object type: integer, floating, pointer (null only), structure, union and array. See [Constexpr objects](#constexpr-objects). |
| `auto` type inference | Supported | At file and block scope, with `const`, `static` and the other specifiers. See [Type inference](#type-inference). |
| `typeof`, `typeof_unqual` | Supported | Of every type name and expression. See [Type inference](#type-inference). |
| Attributes `[[...]]` | Supported | In every position the standard allows. See [Attributes](#attributes). |
| `[[deprecated]]` | Supported | A use warns: `'g' is deprecated [--warn=deprecated-declarations]`. A message argument is accepted and not printed. |
| `[[fallthrough]]` | Supported | Accepted. EmbCC does not warn about fall-through. |
| `[[maybe_unused]]` | Supported | Suppresses `--warn=unused-variable`, `--warn=unused-function` and, on a parameter, `--warn=unused-parameter` for the declaration. |
| `[[nodiscard]]` | Supported | Discarding the result warns: `result of 'f' is discarded, and it is declared warn_unused_result [--warn=unused-result]`. A message argument is accepted and not printed. |
| `[[noreturn]]`, `[[_Noreturn]]` | Supported | |
| `[[unsequenced]]`, `[[reproducible]]` | Supported | Accepted on a function. EmbCC infers what a function reads and writes itself, and eliminates no call on the strength of the promise. |
| `__has_c_attribute` | Supported | The standard attributes' dates (`__has_c_attribute(nodiscard)` is `202003L`), and 0 for an attribute EmbCC does not know. |
| `__has_include` | Supported | |
| `#elifdef`, `#elifndef` | Supported | |
| `#warning` | Supported | Prints `warning: #warning: TEXT`. |
| `#embed` | Supported | With the `limit`, `prefix`, `suffix` and `if_empty` parameters. See [Embedding binary data](#embedding-binary-data). |
| `__has_embed` | Supported | `__STDC_EMBED_FOUND__`, `__STDC_EMBED_EMPTY__` or `__STDC_EMBED_NOT_FOUND__`, with the same parameters. |
| `__VA_OPT__` | Supported | |
| `true` and `false` in `#if` | Supported | |
| Binary constants `0b1010` | Supported | Also in `#if`. |
| Digit separators `1'000'000` | Supported | In integer and floating constants, and in `#if`. |
| `u8` character constants `u8'a'` | Supported | Of type `unsigned char`. |
| `char8_t`; `u8"..."` of type `unsigned char[]` | Supported | Under `--std=c23`, `u8"..."` is `unsigned char[]`, and `<uchar.h>` declares `char8_t`, `mbrtoc8` and `c8rtomb`. Before C23 it is C17's `char[]`. |
| `_BitInt(N)`, the `wb` and `uwb` suffixes | Supported | N up to 65535, which is `BITINT_MAXWIDTH`. Arithmetic in the type wraps at N bits, a `_BitInt` is not promoted to `int`, and conversions rank it by its width. Laid out and passed as each target's ABI says, so calls between EmbCC and GCC agree. See [Integers](implementation-defined.md). |
| Enumerations with a fixed underlying type, `enum E : unsigned char` | Partial | The enumeration and its constants have the underlying type, and `enum E : short;` declares it ahead of its definition. A value the underlying type cannot represent is not diagnosed. See [Enumeration constants](#enumeration-constants). A non-integer type: `an enum's underlying type must be an integer type`. |
| Enumeration constants outside the range of `int` | Supported | The enumeration and its constants take a type that can represent every value, `unsigned long long` for one of 2^63 or more. See [Enumeration constants](#enumeration-constants). |
| Empty initializer `= {}` | Supported | Zero-initializes any object, a VLA included. See [Initializers](#initializers). |
| Labels before declarations and at the end of a compound statement | Supported | |
| Unnamed parameters in a function definition | Supported | `void on_tick(void *) { ... }`. The parameter takes its argument's place and has no name in the debug information; `--warn=unused-parameter` does not report it. |
| Redefinition of a tag with the same content | Supported | A second definition with the same members is the same type. |
| Storage-class specifiers in compound literals, `(static int[]){ 1 }` | Supported | `static`, `constexpr`, `register` and `thread_local`. A `static` one has static storage, so a function may return its address. |
| `()` means `(void)`; K&R definitions removed | Supported | See [Empty parameter lists](#empty-parameter-lists). |
| Trigraphs removed | Supported | EmbCC never replaces trigraphs. |
| `_Decimal32`, `_Decimal64`, `_Decimal128` | Supported | IEEE 754 decimal in the BID encoding, as GCC uses on x86-64 and AArch64, with the `df`, `dd` and `dl` suffixes. The arithmetic is in `librt`. |
| `_Float32`, `_Float64` | Supported | The same types as `float` and `double`, with the `f32` and `f64` constant suffixes. |
| `_Float128` | Partial | The same type as `long double` where that is IEEE binary128. See [Target-dependent features](#target-dependent-features). |
| `_Float16` | Supported | IEEE binary16 on every target, each operation rounded to it; the `f16` constant suffix. Through `...` it is refused. GNU's `__bf16` (bfloat16) too. See [Half-precision floating point](#half-precision-floating-point). |
| `_Float32x`, `_Float64x` | Supported | `double`, and the target's `long double` where that is wider than `double`; the `f32x` and `f64x` suffixes. |
| `unreachable()` in `<stddef.h>` | Supported | Under `-std=c23`; it is `__builtin_unreachable()`. |
| Width macros `INT_WIDTH`, `INT8_WIDTH`, ... | Supported | `<limits.h>` and `<stdint.h>`. |
| `<stdbit.h>`, `<stdckdint.h>` | Supported | Freestanding headers; see [Libraries](libraries.md). |
| `__STDC_VERSION__` of `202311L` | Supported | Under `--std=c23` or `gnu23`; the default, `gnu17`, is `201710L`. |
| Identifiers per Unicode UAX #31 | Supported | An identifier starts with an `XID_Start` character or `_`, and continues with `XID_Continue` characters, from Unicode 16.0, written in UTF-8 or as universal character names. |

### Later drafts

These features of the C2y working draft are supported, under every
`--std=`:

| Feature | Example |
|---|---|
| `_Countof(a)`, the number of elements of an array | `_Countof(a) == 4` |
| The octal prefix `0o` | `0o17` is 15 |
| Delimited escapes | `'\o{101}'`, `'\x{41}'`, `'\u{1F600}'` |
| Declarations in `if` and `switch` | `if (int x = g(); x > 0)` |
| A type name as the controlling operand of `_Generic` | `_Generic(int, int: 1, default: 0)` |

Case ranges, `case 1 ... 3:`, are accepted as a GNU extension; see
[Extensions](extensions.md).

## Target-dependent features

The language features whose availability depends on the target. The
targets are described in [Targets](targets.md): "x86-64" covers the ELF,
EmbLinkOS and Linux triples, "AArch64" covers the ELF, EmbLinkOS and
Linux triples, and "Cortex-M" covers every `thumb*` triple.

| Feature | x86-64 | x86-64 macOS | x86-64 Windows | AArch64 | Apple arm64 | Cortex-M | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|---|---|---|
| Variable length arrays | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes | No |
| `long double` format | x87 80-bit, 16 bytes | x87 80-bit, 16 bytes | x87 80-bit, 16 bytes | binary128, 16 bytes | binary64, 8 bytes | binary64, 8 bytes | binary128, 16 bytes | binary128, 16 bytes | binary32, 4 bytes |
| `long double` arithmetic | Yes | Yes | Not as a parameter or result | Yes, in software | Yes | Yes | No | No | Yes |
| `float` and `double` `_Complex` | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| `long double _Complex` | Yes | Yes | No | Yes | Yes | Yes | No | No | Yes |
| `__int128` | Yes | Yes | Not in a function signature | Yes | Yes | No | No | Declarations and `sizeof` only | No |
| `_Float128` | No | No | No | Yes | No | No | Declarations and `sizeof` only | Declarations and `sizeof` only | No |
| `_Atomic` operators | 1 to 8 bytes | 1 to 8 bytes | 1 to 8 bytes | 1 to 8 bytes | 1 to 8 bytes | 1 to 8 bytes; 8 by a `lib/rt` call | 1 to 8 bytes; 8 by a `lib/rt` call | 1 to 8 bytes | 1 to 8 bytes, with interrupts masked |
| `_Thread_local` | Per thread | Per thread | Per thread (emutls) | Per thread | Per thread | One shared instance | One shared instance | One shared instance | One shared instance |

"One shared instance" means the object is placed in `.tbss` but addressed
as an ordinary static object; see [Targets](targets.md#summary).

The errors for the "No" entries:

| Case | Diagnostic |
|---|---|
| `long double` or `_Float128` arithmetic on RISC-V, `__int128` arithmetic on RV64 | `the RV64 backend cannot lower a 128-bit value yet (function f) [...]` (`RV32` on RV32) |
| `long double` in a signature on Windows | `long double in the signature of 'f' is not supported for a Windows target yet: there it travels by reference and returns through a hidden pointer, and EmbCC passes it on the stack by value` |
| `__int128` on a 32-bit target or AVR | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| `_Float128` on x86-64 | `` _Float128 is not supported on x86-64: `long double` here is x87's 80-bit extended format, not IEEE binary128, so it is not the same type `` |
| `_Float128` where `long double` is not 16 bytes | `_Float128 is not supported on this target: it has no 128-bit floating-point type` |
| An `_Atomic` object wider than the machine moves at once (16 bytes) | `an atomic access of 16 bytes is not one access on this target (it moves 8 at once): the halves could be split by an interrupt or another core` |

The text in square brackets names the internal operation that could not
be lowered.

## Feature notes

### Functions that can reach their closing brace

A function with a non-`void` return type should end every path with a
`return` statement. The standard allows control to reach the closing
brace as long as the caller does not use the value, so EmbCC accepts it,
with a warning that is on by default (`--error=return-type` makes it an
error):

```text
warning: control may reach the end of 'f', which returns int [--warn=return-type] [E0008]
```

The function returns 0 when control reaches the brace, if its result is
an integer or a pointer, rather than whatever its return register held.
A caller that uses the value then reads the same thing every time. A
floating or aggregate result is undefined there.

`main` is the exception the standard makes: reaching its closing brace
returns 0, without a warning.

A path also ends at an infinite loop, at a call to a function declared
`_Noreturn`, `[[noreturn]]` or `__attribute__((noreturn))`, and at a call
to `exit`, `_Exit`, `abort`, `__builtin_unreachable` or
`__builtin_trap`. `embcc --explain E0008` describes the rule; see
[Diagnostics](diagnostics.md#t6).

### Case labels in nested statements

A `case` or `default` label can be anywhere in its `switch` statement's
body: in a nested block, in an `if`, or inside a loop, which the `switch`
then enters in the middle (Duff's device). This follows C11 6.8.4.2. A
nested `switch` has its own labels. Duplicate values and a second
`default` are errors at any nesting depth, and a label with no `switch`
around it is an error (`'case' outside of a switch`).

### Empty parameter lists

Under `--std=c23`, a function declarator with an empty parameter list,
`int f();`, declares a function with no parameters. Before C23 it means
"parameters not specified", and a call to it is not checked. Under C23,
code that relies on the older meaning is refused:

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

A structure, union or enumeration tag declared in a block belongs to that
block, as the standard specifies: two functions may each define
`union u`, and an inner block's `struct s { ... }` declares a new type
that hides an outer `struct s` until the block ends (CMSIS's
`cmsis_gcc.h` defines `union llreg_u` inside each of its 64-bit DSP
intrinsics).
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
warning: initialization discards the 'const' qualifier of const char * [--warn=discarded-qualifiers]
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
`--enums=short` is refused (see [Targets](targets.md#data-models)).

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
  call at `--opt=none`, and any use of its address refer to the external
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

GNU89 semantics apply instead with `-fgnu89-inline`, with `--std=c89`,
`--std=c90`, `--std=iso9899:1990`, `--std=gnu89` or `--std=gnu90`, and for
a function with `__attribute__((gnu_inline))`. Under them a definition
that says `inline` alone is an external definition, and one that says
`extern inline` is used only for inlining and never emitted. See
[`-fgnu89-inline`](invoking.md#-fgnu89-inline--fno-gnu89-inline).

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

GNU's integer complex types, `_Complex int`, `short _Complex`,
`unsigned long long _Complex` and the rest, are supported too, and so is
an integer imaginary constant, `2i`, which is a `_Complex int`. `*` and
`/` of two of them are computed in the integer type, as GCC computes
them. A product is `(ac - bd) + (ad + bc)i`. A quotient uses GCC's scaled
method, each division truncating: when `|c| < |d|`, with `r = c / d`, it
is `((ar + b) + (br - a)i) / (cr + d)`; otherwise, with `r = d / c`,
`((br + a) + (b - ar)i) / (dr + c)`. So `(3+4i) / (1-2i)` is `-2+1i`,
as GCC has it; clang's formula gives `-1+2i`.

A complex object with static storage duration takes an initializer that
EmbCC can fold. Otherwise:

```text
error: a static complex initializer must be a constant that EmbCC folds: literals, casts, + and -, and scaling by a real
```

This rules out a product or quotient of two complex values, and
`CMPLX(1.0, 2.0)`, which `<complex.h>` defines as a call to a library
function. At block scope both are accepted.

### Half-precision floating point

`_Float16` (C23 Annex X) is IEEE 754 binary16 on every target: two
bytes, aligned like a `short`, with an 11-bit significand. An operation
on `_Float16` operands has type `_Float16`, and each one is rounded to
binary16. EmbCC computes it in `float` and rounds the result, which is
exact, because a float's 24 bits are enough that rounding twice never
differs from rounding once. So a program gets the answers of a machine
with binary16 arithmetic, such as Arm's FEAT_FP16 or RISC-V's Zfh, on
every target. `FLT_EVAL_METHOD` is 0.

GCC and clang on x86-64 evaluate instead in `float`, and round only at an
assignment, a cast or a return. They may evaluate a constant in `float`
too: `0.1f16 + 0.2f16` is `0x34cc` with EmbCC and `0x34cd` with GCC
there. Where the two must agree, cast each intermediate result and
constant to `_Float16`.

A constant takes the `f16` or `F16` suffix, and is rounded once from its
digits. A conversion from `double` or `long double` is rounded once from
that value, never through `float`.

`__fp16` is Arm's storage format: the same binary16 bits, but a value
read from one is a `float`, as a `char` read is an `int`. `a + b` of two
`__fp16` is a `float`, rounded once when it is stored, and through `...`
an `__fp16` is a `double`. `_Float16` and `__fp16` are distinct types to
`_Generic`.

`__bf16` is bfloat16: the top half of a `float`, with its 8-bit exponent
and an 8-bit significand, in two bytes. It works as `_Float16` does, on
every target: an operation on two of them, or on one and an integer, is a
`__bf16`, each rounded to bfloat16; with a `float` it is a `float`; and
with a `_Float16` it is a `_Float16`, as in GCC and clang. A constant
takes GCC's `bf16` or `BF16` suffix. A conversion to `__bf16` from
`double`, `long double` or an integer is rounded once: through a `float`,
2^24 + 2^16 + 1 would land on a half-way point and round down. The
conversions are inline integer arithmetic, so no runtime routine is
called. The exception is a 64-bit integer on AVR and RX, whose `double`
is four bytes; there, `__embcc_ll2bf` and `__embcc_ull2bf` in
`lib/rt/half.c` do it. The `__BFLT16_MANT_DIG__` family of predefined
macros describes it. A `__bf16` travels between functions as a
`_Float16` does.

The predefined `__FLT16_MANT_DIG__`, `__FLT16_MAX__` and the rest of that
family are always defined. `<float.h>` defines `FLT16_MANT_DIG` and the
other `FLT16_` names when `__STDC_WANT_IEC_60559_TYPES_EXT__` is defined
before it is included.

A `_Float16` argument or result travels as the target's calling
convention passes one, so a function compiled by EmbCC can call or be
called by one compiled by GCC or clang:

| Target | A `_Float16` argument or result |
|---|---|
| x86-64, AArch64, ARM with `-mfloat-abi=hard` | In the low 16 bits of the floating-point argument register |
| RISC-V with `-mabi=ilp32f`, `ilp32d`, `lp64f` or `lp64d` | NaN-boxed in a floating-point register |
| RISC-V and LoongArch soft-float | Sign-extended in an integer register |
| ARM soft-float, and targets whose convention has no `_Float16` (AVR, MIPS, Xtensa, TriCore, RX, PowerPC, SPARC, ColdFire) | Zero-extended in an integer register |

A struct holding a `_Float16` is passed like any other struct. On AArch64
a struct of two to four of them is a homogeneous aggregate, in `h0` to
`h3`. With the ARM hard-float convention, which also gives each member a
floating-point register of its own, it is refused:

```text
error: 'f': a struct holding a _Float16, passed or returned by value, is not supported yet with the hard-float calling convention, which puts each binary16 member in a floating-point register of its own; pass a pointer to it
```

The same refusal applies to RISC-V's hard-float conventions. A
`_Float16` passed through `...` is refused on every target, because it
is not promoted there:

```text
error: 'printf': passing a _Float16 through '...' is not supported yet: it is not promoted there, and EmbCC does not yet place an unpromoted binary16 among the variable arguments; convert it to double, as va_arg will read it
```

The conversions between binary16 and the wider formats are calls to
EmbCC's runtime, `lib/rt/half.c`. They use integer arithmetic only, so a
target without a floating-point unit pays for no soft-float call there.
On AArch64 each conversion is one `fcvt` instruction instead. The
routines are named as libgcc names them where libgcc has them:
`__extendhfsf2`, `__truncsfhf2` and `__truncdfhf2` on x86-64, and
`__gnu_h2f_ieee`, `__gnu_f2h_ieee` and `__gnu_d2h_ieee` on ARM. On the
other targets the names are EmbCC's own, `__embcc_h2f` and the rest, so a
program linked against another runtime gets a link error naming the
routine. On macOS the system's compiler-rt provides them.

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
compound assignments) are atomic for integer and pointer types, for
`float`, `double` and an 8-byte `long double`, and for structs, as GCC
and clang make them:

- A floating object is loaded, stored and exchanged as the integer of its
  bits; `+=`, `*=`, `/=`, `++` and the rest are a compare-and-swap loop on
  them.
- A struct whose size is a power of two that one access moves is that
  integer. `_Atomic` aligns such a struct to its size -- or to the most
  the target aligns anything to, if that is less -- as GCC lays it out:
  `_Atomic struct { char a, b, c, d; }` is 4-aligned.
- A struct of any other size is read and written whole through
  libatomic's generic `__atomic_load` and `__atomic_store`, which take
  the size first. lib/rt provides them (interrupts masked on a 32-bit
  target; a lock per object's address on a 64-bit one), weak, so an SDK
  or RTOS can replace them.

A struct's value is a copy: reading an `_Atomic struct` gives a snapshot,
and writing to a member of the copy leaves the object as it was. A
`long double` wider than 8 bytes is refused:

```text
error: a compound assignment to an _Atomic long double is not supported: EmbCC makes the operators atomic for integers, pointers, floating types of up to 8 bytes and structs
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

`constexpr` declares a named constant, at file scope or in a block. An
integer one is usable wherever an integer constant expression is, and so
is a member of a `constexpr` structure or union:

```c
constexpr int N = 4;
constexpr double SCALE = 0.25;
constexpr struct { int w, h; } SIZE = { 320, 240 };
int table[N];
static_assert(SIZE.w * SIZE.h == 76800);
```

The object is `const`, and has storage like any other, so its address can
be taken.

| Case | Diagnostic |
|---|---|
| A value the type cannot represent exactly | `constexpr 'c': 300 does not fit unsigned char, and C23 wants it exactly` |
| A non-constant initializer | `constexpr 'k' needs a constant initializer; this one is not one` |
| No initializer | `constexpr 'k' needs an initializer` |
| Assignment to the object | `assignment of read-only 'k' (its type is const int)` |

### Type inference

`typeof(...)` and `typeof_unqual(...)` accept any type name or
expression. `auto` (C23) and `__auto_type` (GNU) declare an object whose
type is that of its initializer, at file scope and in a block. The type
follows the usual conversions: `typeof(c + c)` with `char c` is `int`,
`typeof(arr)` keeps the array type, and `auto a = arr;` is a pointer, as
in C23.

```c
auto limit = 1000u;                 /* unsigned int */
static auto table = (int[]){ 1, 2, 3 };   /* int * */
typeof_unqual(const volatile short) s;     /* short */
```

`auto` needs an initializer: `auto x;` gives `expected a type in this
declaration, found 'auto'`. With several declarators, `auto a = 1, b =
2.0;`, each infers its own type. `auto int x;` is the C89 storage class.

### Attributes

The `[[...]]` syntax shares its implementation with
`__attribute__((...))`: `[[gnu::NAME]]` is the GNU attribute `NAME`, and
the standard attributes map onto GNU ones (`maybe_unused` to `unused`,
`nodiscard` to `warn_unused_result`, `_Noreturn` to `noreturn`). Any
attribute namespace is dropped, so `[[gnu::packed]]` and `[[packed]]`
are the same attribute. The GNU attributes and how each is handled are
listed in [Extensions](extensions.md).

`[[...]]` is accepted in every position C23 gives it:

- at the start of a declaration, at file scope and in a block;
- after `struct`, `union` or `enum`: `struct [[gnu::packed]] S { ... };`;
- after a declarator's identifier, `int x [[maybe_unused]];`, and after an
  array declarator, `int a[3] [[maybe_unused]];`;
- on an enumerator, `enum E { OLD [[deprecated]], NEW };`;
- before a parameter declaration and after its declarator;
- after a function declarator: `void f(void) [[gnu::noinline]];`;
- after a `*`, `int * [[gnu::aligned(8)]] p;`;
- before a statement, including the empty attribute statement
  `[[fallthrough]];` and an empty list `[[]]`.

An attribute at the start of a declaration that declares no object or
function, `[[gnu::packed]] struct S { ... };`, applies to nothing and is
ignored without a diagnostic. Put a type attribute after the `struct`
keyword.

An attribute EmbCC does not know is ignored with a `--warn=attributes`
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

The standard parameters are supported:

| Parameter | Effect |
|---|---|
| `limit(N)` | At most `N` bytes |
| `prefix(TOKENS)` | `TOKENS` before the bytes, when there are any |
| `suffix(TOKENS)` | `TOKENS` after the bytes, when there are any |
| `if_empty(TOKENS)` | `TOKENS` instead, when there are no bytes |
| `gnu::offset(N)`, `clang::offset(N)` | Skip the first `N` bytes |

```c
static const char banner[] = {
#embed "banner.txt" suffix(, 0)     /* a NUL-terminated string */
};
```

The file name may be given by a macro, `#embed LOGO`. A vendor parameter
EmbCC does not know is refused, since ignoring one could embed the wrong
bytes: `#embed parameter 'acme::thing' is not one EmbCC has (limit,
prefix, suffix, if_empty, gnu::offset)`. `__has_embed("file" params)` is `__STDC_EMBED_FOUND__` (1),
`__STDC_EMBED_EMPTY__` (2) or `__STDC_EMBED_NOT_FOUND__` (0).

| Case | Diagnostic |
|---|---|
| A file that is not found | `cannot find the file to embed: "logo.bin"` |

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
command-line options that control it (`-D`, `-U`, `-I`, `--system-include`, the
dependency options) are in
[Invoking EmbCC](invoking.md#headers-and-macros). The GNU directives and
operators (`#include_next`, `__has_include_next`, `__has_builtin`,
`__has_attribute`, `#pragma pack`, ...) are in [Extensions](extensions.md).

### Translation phases

| Phase | Status | Notes |
|---|---|---|
| Source character set | Supported | Source files are read as UTF-8. A leading byte-order mark is skipped. |
| Trigraphs | Not supported | Never replaced. |
| Line splicing | Supported | A backslash-newline is removed everywhere, as translation phase 2 says: between tokens, in directives, inside identifiers, numbers, string literals and character constants, and at the end of a `//` comment, which then continues on the next line, and `--warn=comment` (GCC's `-Wcomment`, in `--warn=all`) says so. |
| Comments | Supported | Each comment is replaced by one space. |
| Digraphs | Supported | See [C95](#c95-amendment-1). |
| Universal character names | Supported | See the [C99](#c99) table. |
| String literal concatenation | Supported | |


### Directives

| Directive | Status | Notes |
|---|---|---|
| `#include "file"`, `#include <file>` | Supported | The search order is in [Invoking EmbCC](invoking.md#search-order). |
| `#include` with a macro-expanded name, `#include HEADER` | Supported | The replacement must be one of the two forms, `"name"` or `<name>`: `#include CMSIS_device_header`. |
| `#define`, `#undef` | Supported | Redefining a macro with a body that differs only in white space is accepted with a warning, `macro 'NAME' redefined`; the standard allows it silently. |
| `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif` | Partial | See [Conditional expressions](#conditional-expressions). |
| `#elifdef`, `#elifndef` | Supported | |
| `#line DIGITS ["file"]` | Supported | A missing or zero line number: `#line needs a positive line number`. |
| `#error TEXT` | Supported | An error: `#error: TEXT`. |
| `#warning TEXT` | Supported | A warning: `#warning: TEXT`. |
| `#pragma` | Partial | `pack`, `once`, `push_macro`, `pop_macro` and `weak` are acted on (see [Extensions](extensions.md#pragmas)). Every other pragma, including the `STDC` pragmas, is ignored without a diagnostic. |
| `_Pragma("...")` | Supported | The same as the corresponding `#pragma`. |
| `#embed` | Supported | With the `limit`, `prefix`, `suffix` and `if_empty` parameters. See [Embedding binary data](#embedding-binary-data). |
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
| Unsigned arithmetic | Every value is a signed 64-bit integer; a `u` suffix is ignored. `#if -1 < 0u` is true; the standard makes it false. |

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
| `__has_c_attribute(NAME)` | Supported | The date of a standard attribute EmbCC has, and 0 for any other; see the [C23](#c23) table. |
| `__has_embed(...)` | Supported | See [Embedding binary data](#embedding-binary-data). |
