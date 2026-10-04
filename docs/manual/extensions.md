# Language extensions

This page describes the extensions to ISO C that EmbCC accepts: the GNU C
language extensions, attributes, builtin functions, the atomic builtins,
the feature-test operators, preprocessor extensions and pragmas. For each
one it says what EmbCC does, on which targets, and the exact diagnostic
when EmbCC refuses it. It is for people porting code written for GCC or
Clang and for people writing code for EmbCC directly. Standard C is
described in [C language](c-language.md), C++ in [C++](cxx.md), and the
`asm` statement in [Inline assembly](inline-asm.md).

## Overview

### Extensions are always enabled

Every extension on this page is available in every compilation.
[`-std=`](invoking.md#-stdstandard) does not change what the parser
accepts, and no warning reports the use of an extension. `-pedantic` and
`-pedantic-errors` are accepted, turn nothing on, and say so:

```text
embcc: warning: -pedantic: EmbCC has no diagnostics for extensions to ISO C, so this turns nothing on
```

When EmbCC does not implement an extension, it refuses the program with
an error that names the construct, rather than compiling it to do
something else. The few places where a construct is accepted and has no
effect, without an error, are stated where they occur and collected in
[Accepted without effect](#accepted-without-effect).

### Target names used on this page

Where support depends on the code generator, this page names targets
as follows. Each name covers every triple of that family; see
[Targets](targets.md).

| Name | Triples |
|---|---|
| x86-64 | `x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu`, `x86_64-apple-darwin`, `x86_64-windows-gnu` |
| AArch64 | `aarch64-elf`, `aarch64-emblink`, `aarch64-linux-gnu`, `aarch64-apple-darwin` |
| Cortex-M | `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf` |
| RV32, RV64 | `riscv32-unknown-elf`, `riscv64-unknown-elf` |
| AVR | `avr` |

A construct that a code generator cannot lower is refused when the
function containing it is compiled, with a message of the form

```text
embcc: f.c:2: error: the RV32 backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]
```

The bracketed part names the internal operation that could not be
lowered.

### Detecting EmbCC

EmbCC predefines `__EMBCC__` as `1`. A C translation unit does not define
`__GNUC__`, `__GNUC_MINOR__` or `__clang__`, so a header that enables GNU
features under `#ifdef __GNUC__` takes its other path when compiled by
EmbCC, unless `-fgnuc-version=` asks for them (CMSIS needs that; see
[Invoking](invoking.md#-fgnuc-versionmajorminorpatch)). A C++ translation unit presents itself as g++ 16.2; see
[C++](cxx.md#compiler-identity).

To test for a feature, use the [feature-test
operators](#feature-test-operators), with the limits described there.

## GNU C language extensions

### Summary

| Extension | Status |
|---|---|
| [Statement expressions](#statement-expressions) `({ ... })` | Supported |
| [`typeof`, `__typeof__`, `__typeof`, `typeof_unqual`](#typeof) | Supported for a type name and for most expressions |
| [`__auto_type`](#__auto_type) | Supported at block scope |
| [Labels as values and computed `goto`](#labels-as-values-and-computed-goto) | x86-64 and AArch64 only |
| [Local labels](#local-labels) (`__label__`) | Supported |
| [Case ranges](#case-ranges) (`case 1 ... 5:`) | Supported |
| [Designated range initializers](#designated-range-initializers) (`[2 ... 5] = x`) | Supported |
| [Zero-length arrays](#zero-length-and-flexible-arrays) | Supported as a structure member |
| [Static initialization of a flexible array member](#zero-length-and-flexible-arrays) | Supported |
| [`__int128`, `__real__`, `__imag__` and other types](#additional-types) | See the section |
| [Alternate keywords](#alternate-keywords) (`__asm__`, `__inline__`, ...) | Supported, except `__signed__`, `__signed`, `__const`, `__const__` |
| `__alignof__` of an expression | Supported |
| `\e` escape (ESC, 27) | Supported |
| Binary integer constants (`0b1011`) | Supported |
| `__FUNCTION__`, `__PRETTY_FUNCTION__` | Supported; both are the function's name, as `__func__` is |
| Anonymous structures and unions as members | Supported |
| [Constructs EmbCC does not support](#gnu-extensions-that-are-not-supported) | Conditional with omitted operand, nested functions, empty structures, arithmetic on `void *`, cast to a union, integer complex types, `$` in identifiers, asm labels, `return` of a `void` expression |

### Statement expressions

A compound statement in parentheses is an expression. Its value is the
value of its last statement, when that statement is an expression
statement; otherwise its type is `void`. The value may be of any type,
including a structure.

```c
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); \
                     _a > _b ? _a : _b; })
```

Statement expressions are supported on every target. One cannot appear
in a static initializer (`a static initializer must be a constant, a
string literal, or the address of a global`). `break` and `continue`
inside one refer to a loop or `switch` inside the same statement
expression; one that would leave it is refused (`'break' outside of a
loop or switch`).

### `typeof`

`typeof`, `__typeof__` and `__typeof` are one keyword. Each accepts a
type name or an expression; the expression is not evaluated. C23's
`typeof_unqual` gives the same type without its qualifiers.

The type of an expression is determined while the declaration is
parsed, which covers the forms that macros use. The operand may be:

- an integer, floating or character constant, or a string literal
  without an encoding prefix other than `u8`;
- the name of an object: a variable in scope or one at file scope;
- a cast, a conditional (`?:`), unary `-`, `~` and `!`, `*`, `&`,
  array subscripting, `.` and `->`;
- a binary arithmetic, shift, comparison or logical operator, including
  pointer arithmetic and the difference of two pointers;
- a call to a function by name, declared earlier, or through a function
  pointer.

Any other expression is refused:

```text
embcc: t.c:1: error: typeof of an unsupported expression
```

This includes the name of a function or its address (`typeof(f)`,
`typeof(&f)`), an enumeration constant, an assignment, `++` and `--`,
the comma operator, `sizeof`, `_Generic`, a compound literal, a
statement expression, and a string literal with an `L`, `u` or `U`
prefix. For a function, write its type as a type name.

### `__auto_type`

`__auto_type name = initializer;` declares `name` with the type of its
initializer, after an array is converted to a pointer. The type is
determined as for [`typeof`](#typeof), so the same expressions are
accepted:

```text
embcc: a.c:1: error: __auto_type cannot see the type of this initializer
```

The declaration must have exactly one plain identifier and an
initializer (`__auto_type needs a plain variable name`, `__auto_type
needs an initializer: there is nothing else to take the type from`). It
is accepted at block scope only; at file scope it is a syntax error:

```text
embcc: a.c:1:1: error: expected a type before ?
```

C23's `auto x = expression;` means the same thing, and is also accepted
at block scope only.

### Labels as values and computed `goto`

`&&label` is the address of a label in the current function, of type
`void *`, and `goto *expression;` jumps to such an address.

```c
int dispatch(int op)
{
    void *table[] = { &&add, &&sub };
    goto *table[op];
add:
    return 1;
sub:
    return 2;
}
```

This is supported on x86-64 and AArch64. The other code generators
refuse it:

| Target | Diagnostic |
|---|---|
| Cortex-M | `the ARMv7-M backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]` |
| RV32 | `the RV32 backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]` |
| RV64 | `the RV64 backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]` |
| AVR | `the AVR backend cannot lower labeladdr yet (function f) [labeladdr w=4 size=4]` |

On every target, a label address cannot initialize an object with
static storage duration, so the table above must be an automatic
array. A `static` table is refused with `a static initializer must be a
constant, a string literal, or the address of a global`. The difference
of two label addresses (`&&b - &&a`) is refused as `arithmetic on void
*`. A label whose address is taken must be defined (`label 'x' used but
not defined`). A function that contains a computed `goto` is never
inlined; see [Inlining](optimization.md#inlining).

### Local labels

`__label__ name;` at the start of a block, or anywhere a statement may
start, declares `name` as a label local to that block. A macro that
contains a label can then be expanded more than once in a function.

```c
#define RETRY(stmt) do { __label__ again; again: if (!(stmt)) goto again; } while (0)
```

Several names may be declared at once (`__label__ a, b;`).

### Case ranges

`case low ... high:` matches every value from `low` to `high`
inclusive. Write spaces around `...`: `case 1...5:` is read as a number
and refused (`malformed floating constant`).

```c
switch (c) {
case '0' ... '9': return DIGIT;
case 'a' ... 'z': return LOWER;
}
```

A range whose bounds are reversed is an error (`case range 5 ... 1 runs
backwards`), and so is a value that two labels cover (`duplicate case
label 3`).

### Designated range initializers

In an array initializer, `[first ... last] = value` initializes every
element from `first` to `last`:

```c
int widths[10] = { [2 ... 5] = 8, [7] = 16 };
```

### Zero-length and flexible arrays

A structure member may be declared as an array of length zero, the
pre-C99 form of a flexible array member. It contributes no size.
An object that is itself an array of length zero is refused:

```text
embcc: z.c:1: error: 'z' has incomplete type int[0]
```

A structure with a flexible array member may be initialized statically,
including its trailing array:

```c
struct msg { int len; char data[]; };
struct msg hello = { 5, { 'h', 'e', 'l', 'l', 'o' } };
```

### Additional types

| Type | Status |
|---|---|
| `__int128`, `unsigned __int128`, `__int128_t`, `__uint128_t` | x86-64 and AArch64. On RV64 the type exists but operations on it are refused (`the RV64 backend cannot lower a 128-bit value yet ...`). On Cortex-M, RV32 and AVR: `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| `_Complex` and `__complex__`, `__real__`, `__imag__` | Supported for `float`, `double` and `long double` on every target |
| Imaginary constants with an `i` or `j` suffix (`2.0i`, `3.0fi`, `1.0j`) | Supported for floating constants. An integer one is refused: `an integer imaginary constant (GNU _Complex int) is not supported — write it as a floating one (2.0i)` |
| Integer complex types (`_Complex int`) | Not supported: `invalid _Complex type (only float, double and long double _Complex are supported)` |
| `_Float32`, `_Float64` | The same types as `float` and `double` |
| `_Float128`, `__float128` | AArch64 ELF and Linux targets, where `long double` is IEEE binary128. Refused on x86-64 (`_Float128 is not supported on x86-64: ...`) and on Apple arm64, Cortex-M and AVR (`_Float128 is not supported on this target: it has no 128-bit floating-point type`); on RISC-V its operations are refused like those of `long double` |
| `_Float16`, `__fp16` | Not supported: ``_Float16/__fp16 is not supported: EmbCC has no 16-bit floating-point type, and widening it to `float` would give 24 bits of mantissa where the program asked for 11`` |
| `_Float32x`, `_Decimal32` and the other interchange and decimal types | Not supported (`expected a type before '_Float32x'`) |

The size, alignment and format of each type per target are in
[Targets](targets.md#data-models).

### Alternate keywords

These spellings are accepted wherever the standard keyword is, so that
headers written for `-ansi` work:

| Spelling | Means |
|---|---|
| `__asm__`, `__asm` | `asm` |
| `__inline__`, `__inline` | `inline` |
| `__volatile__`, `__volatile` | `volatile` |
| `__restrict__`, `__restrict` | `restrict` |
| `__typeof__`, `__typeof` | `typeof` |
| `__alignof__`, `__alignof` | `_Alignof`; the operand may also be an expression |
| `__complex__`, `__complex` | `_Complex` |
| `__real`, `__imag` | `__real__`, `__imag__` |
| `__attribute` | `__attribute__` |
| `__thread` | `_Thread_local` |
| `__extension__` | Nothing; it is predefined as an empty macro |

`__signed__`, `__signed`, `__const` and `__const__` are not recognized;
a declaration that uses them is a syntax error (`expected a type before
'__signed__'`).

### GNU extensions that are not supported

Each of these is refused with the diagnostic shown.

| Construct | Diagnostic |
|---|---|
| Conditional with the middle operand omitted, `x ?: y` | `expected an expression, got ':'` |
| Nested function definitions | `expected ';' before '{' [E0002]`, at the inner function's body |
| A structure or union with no members | `a struct/union needs at least one member` |
| Arithmetic on `void *` | `arithmetic on void *`, and `++/-- on void *` |
| Arithmetic on a function pointer | `arithmetic on function *` |
| `sizeof(void)` | `sizeof(void)` |
| Cast to a union type, `(union u)x` | `cannot cast to union u` |
| `$` in an identifier | `character '$' is not supported yet` |
| An asm label, `int f(void) __asm__("name");` | `expected '{' or ';' before 'asm'` |
| `return f();` in a `void` function, where `f` returns `void` | `returning a value from void 'g'` |
| A conditional or cast as an lvalue | `assignment target must be a variable, *pointer, or member` |
| Vector types; see [Vector extensions](#vector-extensions) | |

`sizeof` applied to a function designator gives the size of a pointer
to the function, not 1 as in GCC.

## Attributes

### Syntax

EmbCC reads attributes in two spellings:

- GNU: `__attribute__((name))` or `__attribute__((name(arguments)))`,
  with several attributes separated by commas;
- C23: `[[name]]`, `[[gnu::name]]` or `[[__gnu__::name]]`. The standard
  names `[[maybe_unused]]`, `[[nodiscard]]` and `[[_Noreturn]]` mean
  `unused`, `warn_unused_result` and `noreturn`; the others coincide
  with their GNU names. The scope is dropped whatever it is, so
  `[[clang::packed]]` also means `packed`.

Each name may also be written with two leading and two trailing
underscores (`__packed__`). The two spellings share one parser, so an
attribute means the same in both.

An attribute may appear:

- at the start of a declaration, before or among the declaration
  specifiers;
- after a declarator, including after a function's parameter list;
- after `struct`, `union` or `enum`, before the tag, or after the closing
  brace of the definition (not between the tag and `{`);
- at the start of a structure member's declaration, or after the
  member's declarator;
- at the start of a parameter's declaration, before its type;
- after a `*` in a declarator, where `weak`, `noreturn`, `format`,
  `packed`, `aligned`, `section` and `pcs` apply to the declaration;
- as a statement on its own (`__attribute__((fallthrough));`,
  `[[fallthrough]];`), and after a label.

These positions are not accepted:

- after a parameter's name, `int f(int x __attribute__((unused)))`:
  `expected ')' before '__attribute__'`;
- a C23 attribute after a declarator's name, `int x [[gnu::aligned(16)]];`:
  `expected an expression, got '['`;
- on an enumerator: `expected '}' before '__attribute__'`;
- after the `*` of a structure member's declarator, for an attribute
  that changes layout or linkage:
  `__attribute__((weak)) is not supported in this position (after a declarator it is; on a struct or union, put it right after the keyword or after the closing '}')`.

### How EmbCC treats an attribute

Every attribute name EmbCC knows has one of four dispositions:

- **Honored**: EmbCC acts on it, as described below.
- **Accepted with no effect**: it asks for something that is already
  true of EmbCC's output, or that only matters to an analysis EmbCC
  does not perform. It is accepted without a diagnostic.
- **Accepted with a warning**: `error` and `warning`. Ignoring them is
  not a miscompilation, but the check they ask for does not happen, so
  EmbCC says so under [`-Wattributes`](diagnostics.md#-wattributes).
- **Refused**: ignoring it would generate different code from what the
  program asks for, so it is an error that names the attribute.

An attribute EmbCC does not know is ignored with a warning, which
`-Wno-attributes` silences:

```text
embcc: attr.c:1: warning: attribute 'frobnicate' is not one EmbCC knows, and is ignored [-Wattributes]
```

### Function attributes

| Attribute | Effect |
|---|---|
| `alias("target")` | The function is another name for `target`, which must be a function defined in the same file and not itself an alias. The declaration with `alias` cannot have a body. Combine with `weak` for a weak alias. ELF only; see [Object formats](#object-formats) |
| `always_inline` | Lifts the inliner's size limit for calls to this function. It has effect only where the inliner runs; see [Inlining](optimization.md#inlining) |
| `constructor`, `destructor` | The function's address is placed in `.init_array` or `.fini_array`, for the startup code to call before `main` or at exit. On a bare-metal target the program's own startup code walks the tables; see [Startup code](embedded.md#startup-code-and-the-linkers-symbols). A `static` one is kept although nothing calls it. ELF only |
| `deprecated`, `deprecated("message")` | A call, a read, or taking the address warns under [`-Wdeprecated-declarations`](diagnostics.md#-wdeprecated-declarations), on by default. The message text is not printed |
| `embcc_sret` | EmbCC's own. On the first parameter, before its type, it marks that parameter as the address of the returned aggregate, passed where the target's ABI passes it. EmbCC's C++ lowering writes it; hand-written C has no need of it |
| `format(archetype, string-index, first-to-check)` | Calls are checked under [`-Wformat`](diagnostics.md#-wformat), which `-Wall` enables. The archetypes checked are `printf`, `gnu_printf`, `scanf` and `gnu_scanf`; any other (`strftime`, `strfmon`) is accepted and not checked. Both indexes are 1-based; `first-to-check` is 0 for a function that takes a `va_list` |
| `gnu_inline` | GNU89 `inline` semantics for this function: a definition that says `extern inline` is used only for inlining and never emitted, and one that says `inline` alone is an external definition. See [Inline functions](c-language.md#inline-functions) |
| `noinline` | The function is never inlined |
| `noreturn`, `_Noreturn`, `[[noreturn]]` | The function does not return. A call to it ends a path for EmbCC's check that every path through a non-`void` function returns a value ([E0008](diagnostics.md#diagnostic-ids)). EmbCC does not check that the function itself never returns |
| `nothrow` | The function throws no C++ exception: a call to it inside a C++ `try` region gets no landing pad |
| `pcs("aapcs")`, `pcs("aapcs-vfp")` | ARM only. See [`pcs`](#pcs) |
| `section("name")` | The function's code is placed in section `name`. See [`section`](#section) |
| `unused` | Suppresses [`-Wunused-function`](diagnostics.md#-wunused-function) for this function |
| `used` | The function is emitted even if nothing in the translation unit refers to it, as for a handler reached only from a table or from assembly |
| `visibility("default" \| "hidden" \| "protected" \| "internal")` | Sets the symbol's ELF visibility. Any other value is refused: `visibility attribute wants "default", "hidden", "protected" or "internal"` |
| `warn_unused_result`, `[[nodiscard]]` | Discarding the result warns under [`-Wunused-result`](diagnostics.md#-wunused-result), on by default |
| `weak` | On a definition, the symbol is weak and another definition overrides it at link time. On a declaration, the reference is weak: the function's address is null if no definition is linked. `weak` on any declaration of a function makes it weak |
| `interrupt`, `signal` | Interrupt handlers. See [Interrupt handlers](#interrupt-handlers) |

A constructor or destructor with a priority is refused, because EmbCC
emits one `.init_array` in source order:

```text
embcc: c.c:1: error: __attribute__((constructor(101))) is not supported: EmbCC emits one .init_array in source order and cannot honour a priority
```

Declarations of one function that disagree are refused: two different
sections (`'f' is placed in section '.a' here and '.b' before`) or two
different `pcs` conventions.

### Variable attributes

| Attribute | Effect |
|---|---|
| `aligned(N)`, `aligned` | The object is aligned to at least `N` bytes; with no argument, to 16 bytes on every target. Applies to file-scope and local variables. See [`aligned`](#aligned) |
| `deprecated` | A use of the variable warns under `-Wdeprecated-declarations` |
| `section("name")` | The object is placed in section `name`. File scope only. See [`section`](#section) |
| `unused` | Suppresses [`-Wunused-variable`](diagnostics.md#-wunused-variable) |
| `used` | A `static` variable is emitted even if nothing refers to it |
| `visibility("...")` | As for functions |
| `weak` | As for functions: a weak definition, or a weak reference whose address is null when nothing defines it |

`alias` on a variable is refused: `alias attribute on variable 'x' is
not supported (functions take it)`.

### Type attributes

| Attribute | On | Effect |
|---|---|---|
| `packed` | `struct`, `union` | Every member's alignment becomes 1: no padding between members, and the structure's alignment is 1. A member's own `aligned(N)` still applies |
| `aligned(N)` | `struct`, `union` | The type's alignment is raised to at least `N`, and its size is rounded up to a multiple of it |
| `aligned(N)` | a member | The member's alignment is raised to at least `N`, which raises the structure's |
| `aligned(N)` | `typedef` | Refused when `N` exceeds the type's alignment; accepted with no effect otherwise. See below |
| `packed`, `aligned` | `enum` | Refused: `a packed or aligned enum is not supported (EmbCC's enums are always int-sized)` |
| `deprecated` | a type | Accepted; using the type does not warn |
| `may_alias` | a type | Accepted with no effect; EmbCC performs no type-based alias analysis |

`packed` applies to a whole structure or union. Written on a single
member, before or after its declarator, it is accepted and has no
effect.

An alignment on a `typedef` name larger than the type's own is refused,
because EmbCC carries alignment on objects and on structure
definitions, not on a type name:

```text
embcc: t.c:1: error: __attribute__((aligned(16))) on a typedef is not supported: EmbCC carries alignment on objects and on struct definitions, not on a type name; put it on the declaration that uses 'i16'
```

A `typedef` of a structure that carries its own `aligned` or `packed`
keeps that layout.

### Statement and label attributes

`fallthrough` (as `__attribute__((fallthrough));` or `[[fallthrough]];`)
is accepted and has no effect; EmbCC does not warn about a `case` that
falls through. `unused` after a label is accepted.

### `aligned`

The argument of `aligned` is an integer constant expression, evaluated
as the operand of `_Alignas` is. `aligned(2*32)` and `aligned((64))`
give 64, and `aligned(sizeof(long long))` gives the size of `long long`:

```c
#define CACHE_LINE 64
struct ring { unsigned head, tail; } __attribute__((aligned(CACHE_LINE)));
struct line { char bytes[CACHE_LINE]; } __attribute__((aligned(2 * CACHE_LINE)));
```

The value must be a positive power of two. Any other value, including 0,
and an argument that is not an integer constant expression are refused:

```text
embcc: a.c:1: error: aligned wants a constant power of two
```

`_Alignas` makes the same check (`_Alignas requires a constant power of
two`), except that `_Alignas(0)` is accepted and has no effect, as C11
specifies.

A local array, structure or union whose alignment exceeds what the
stack pointer guarantees (16 bytes on x86-64, AArch64 and RISC-V, 8 on
Cortex-M) is placed in storage that EmbCC aligns at function entry, so
its address has the requested alignment at any call depth. A local of
scalar type with such an alignment is refused:

```text
embcc: f.c:2:20: error: 'x' needs 64-byte alignment and the stack only guarantees 16: supported for an array or a struct, not yet for a scalar
```

On AVR, a local variable cannot be given an alignment:

```text
embcc: f.c:1: error: the AVR backend cannot lower a local with __attribute__((aligned)): AVR's stack pointer has no known alignment, so a frame slot cannot be given one yet (function f)
```

`_Alignas` and `#pragma pack` interact with `aligned` as in GCC: the
stricter of `aligned(N)` and `_Alignas(N)` applies, and `#pragma
pack(N)` caps every member's alignment at `N`, including one raised by
a member's `aligned`.

### `section`

`section("name")` places a file-scope variable or a function in the
named section of the object file.

For a variable:

- A name beginning `.text` makes the section executable, and one
  beginning `.rodata` read-only. Any other name gives a writable
  section.
- `.bss` and names beginning `.bss.` give a section with no file
  contents (NOBITS). An object placed there cannot have an initializer:
  `'z' has an initializer but is placed in NOBITS section '.bss.x'`.
- A `const` object with a section goes to that section, not to
  `.rodata`.
- An object without an initializer is zero-filled in its section.
- A thread-local object cannot have a section: `'z' is __thread and
  also names a section: a thread-local object has to be in .tdata or
  .tbss, which is what makes it per-thread`.
- One translation unit can use at most 64 named data sections.

For a function, the code is placed in the named section, and the
functions that share a section are emitted together. A function section
cannot be used with `-g` (`-g with a function in a section of its own
('.ramfunc') is not supported yet: the compile unit's address range
would span two sections`), and a function and a variable cannot share a
section name (`section '.shared' holds a function, and 'd' cannot share
it: one is code, the other data`).

`section` on a block-scope variable is refused:

```text
embcc: s.c:1: error: section attribute on block-scope 'x' is not supported — declare it at file scope
```

On a structure member it is accepted and has no effect.

### `pcs`

`pcs("aapcs")` and `pcs("aapcs-vfp")` select the ARM procedure-call
standard for one function, whatever `-mfloat-abi` selects for the rest
of the program: `aapcs` passes floating-point values in core registers,
and `aapcs-vfp` in VFP registers. Runtime libraries put `pcs("aapcs")`
on helpers that must keep the base convention under
`-mfloat-abi=hard`. See [Calling convention:
AAPCS32](targets.md#calling-convention-aapcs32).

| Condition | Diagnostic |
|---|---|
| Not an ARM target | `__attribute__((pcs)) names an ARM calling convention, and this is not an ARM target` |
| Any other value | `pcs wants "aapcs" or "aapcs-vfp"` |
| `aapcs-vfp` without an FPU | `pcs("aapcs-vfp") passes floating point in VFP registers, and this part has no FPU: add -mfpu=` |
| On a variable, parameter, member or `typedef` | `pcs is only supported on a function declaration, not on a variable` (or `a parameter`, `a member`, `a typedef`) |
| On a function pointer that is a structure member | `pcs is only supported on a function declaration, not on a pointer to one: a call through the pointer would use the default convention` |
| Two declarations disagree | `'f' is declared with a different pcs than on line 3` |
| Taking the address of a function whose `pcs` differs from the build's convention | `taking the address of 'f' is not supported: it is declared with a pcs attribute that is not this build's convention, and a call through the pointer would use this build's` |

On a function-pointer parameter, `pcs` is accepted and has no effect.

### Attributes accepted with no effect

These are accepted without a diagnostic. The reason is the one EmbCC
records for each.

| Attribute | Why it has no effect |
|---|---|
| `access` | It describes pointer-argument accesses for warnings EmbCC does not issue |
| `alloc_align`, `alloc_size`, `counted_by` | EmbCC has no object-size checking |
| `artificial` | It marks a function for a debugger's stepping |
| `assume_aligned` (attribute) | It promises a returned pointer's alignment, which only a pass that widens accesses could use |
| `cold`, `hot` | EmbCC does not reorder code by frequency |
| `const`, `pure` | EmbCC does not eliminate repeated calls; it infers whether a function reads or writes memory by itself |
| `copy` | The attributes worth copying are recorded on the declaration itself |
| `designated_init` | It asks for a warning; the layout is unaffected |
| `fallthrough` | EmbCC does not warn about a `case` falling through |
| `flatten` | EmbCC's inliner works from the call site |
| `leaf` | Nothing in EmbCC reasons across a call this way |
| `malloc` | It says the result aliases nothing, which only an alias analysis could use |
| `may_alias` | EmbCC does no type-based alias analysis |
| `no_instrument_function` | EmbCC emits no instrumentation calls |
| `no_sanitize`, `no_sanitize_address`, `no_sanitize_undefined` | EmbCC has no sanitizers of these kinds |
| `noclone` | EmbCC never clones a function |
| `noipa` | The only interprocedural pass is the inliner, which `always_inline` and `noinline` control |
| `nonnull`, `returns_nonnull` | EmbCC does not track or check null pointers |
| `optimize` | The optimization level applies to the whole compilation, not per function |
| `returns_twice` | See below |
| `sentinel` | EmbCC does not check variadic terminators |
| `tls_model` | EmbCC emits one thread-local model |

`returns_twice` is accepted because newlib's `<setjmp.h>` puts it on
`setjmp`, and is not acted on. Above `-O0`, a local variable that the
optimizer keeps in a register across a call to such a function can hold
a stale value after the second return. A `volatile` local is kept in
memory at every optimization level, so a value stored in it between the
two returns is still there after the second, as C requires; declare a
local `volatile` when it is changed after `setjmp` and read after
`longjmp`.

`error("message")` and `warning("message")` are accepted with a warning,
because GCC reports a call to such a function only after optimization,
and EmbCC does not:

```text
embcc: e.c:1: warning: __attribute__((error)) is accepted but does nothing here: it makes a CALL to this function a compile error unless the optimizer removes the call, so the diagnostic has to wait until after optimisation and EmbCC issues its own before then; a build-time assertion written with it will not fire [-Wattributes]
```

A build-time assertion written with `error` therefore does not fail the
build. Use `_Static_assert`.

### Attributes that are refused

Each of these is an error of the form `__attribute__((NAME)) is not
supported: REASON`:

| Attribute | Reason given |
|---|---|
| `cleanup` | `the cleanup function would never run` |
| `ifunc` | `the resolver would never run and calls would go to it rather than to the implementation it picks` |
| `interrupt` | On x86-64, AArch64 and RISC-V; see [Interrupt handlers](#interrupt-handlers) |
| `mode` | `the declaration would keep its written type, so a typedef that asks for a specific width would silently get another` |
| `ms_abi` | `the arguments would be passed in System V's registers` |
| `naked` | `the prologue the function says it must not have would be emitted anyway, and its own asm would run on a frame it did not set up` |
| `signal` | On every target but AVR; see [Interrupt handlers](#interrupt-handlers) |
| `sysv_abi` | `the arguments would be passed in the other convention's registers` |
| `target` | `EmbCC selects its instruction set per compilation; a function asking for another would be compiled for the wrong one` |
| `transparent_union` | `the union would be passed as a union rather than as its first member, which is a different calling convention` |
| `vector_size` | `the type would stay a scalar: ...`; see [Vector extensions](#vector-extensions) |
| `weakref` | `the symbol would be emitted as an ordinary reference, so a missing target would fail to link instead of being null` |

`ms_abi` and `sysv_abi` are refused on every target, including
`x86_64-windows-gnu`.

### Object formats

Some attributes need support from the object-file writer.

| Attribute | ELF | Mach-O | COFF |
|---|---|---|---|
| `section` on a variable | Supported | The object goes in segment `__DATA`, in a section with the name as written. A `"segment,section"` name is not split | `'b': a variable's section attribute is not supported for COFF output` |
| `section` on a function | Supported | `a function's section attribute is not supported for Mach-O output` | `a function's section attribute is not supported for COFF output` |
| `alias` | Supported | `alias attribute on 'f' is not supported for Mach-O output` | `alias attribute on 'f' is not supported for COFF output` |
| `constructor`, `destructor` | `.init_array`, `.fini_array` | `__attribute__((constructor)) is not supported for a Darwin target yet: it needs a __DATA,__mod_init_func section this Mach-O writer does not emit` | `__attribute__((constructor)) is not supported for a Windows target yet: it needs the .ctors/.dtors sections this COFF writer does not emit` |
| `weak` | Supported | Supported | Accepted with no effect |
| `visibility` | Supported | Accepted with no effect | Accepted with no effect |

## Embedded programming attributes

This section collects the attributes used in firmware. How to write a
vector table, a startup routine and handlers for each board is in
[Embedded programming](embedded.md).

### Interrupt handlers

| Target | `__attribute__((interrupt))` | `__attribute__((signal))` |
|---|---|---|
| AVR | Implemented. The handler saves `r0`, `SREG`, `r1`, the call-clobbered registers and the frame pointer, clears `r1`, re-enables interrupts with `sei` on entry, and returns with `reti` | Implemented, as `interrupt` without the `sei`: interrupts stay disabled in the body |
| Cortex-M | Accepted; the code is the same as without it, because the processor saves the caller-saved registers on exception entry and an ordinary return performs the exception return. An argument such as `interrupt("IRQ")` is accepted | Refused |
| RISC-V, x86-64, AArch64 | Refused | Refused |

The refusals read:

```text
embcc: isr.c:2: error: __attribute__((interrupt)) is not supported: the handler would return with an ordinary return instead of the interrupt return the CPU needs, and without saving the registers (on ARMv7-M it needs neither, and is accepted; on AVR it is implemented)
embcc: isr.c:2: error: __attribute__((signal)) is not supported: an interrupt handler needs the machine's own return instruction and every register saved, which only the AVR backend does
```

On RISC-V, write the trap entry in assembly and call a C function from
it; see [Trap handlers](embedded.md#trap-handlers). For the AVR vector
names (`__vector_N`), see [AVR](embedded.md#avr-atmega328p).

`naked` is refused on every target, so a function that must run without
a prologue (a reset handler that sets up the stack) is written in
assembly.

### Placing code and data

`section` puts a vector table, a function that must run from RAM, or a
table collected by the linker into a named section:

```c
void reset(void);

__attribute__((section(".vectors"), used))
void (*const vectors[])(void) = { (void (*)(void))0x20010000u, reset };

__attribute__((section(".ramfunc"))) void flash_write(void) { /* ... */ }
```

Add `used` to a table that nothing in the program refers to by name, so
that it is emitted.

### Default handlers

`weak` together with `alias` gives the CMSIS pattern in which every
interrupt handler defaults to one function and the application
overrides the ones it needs:

```c
void Default_Handler(void) { for (;;) { } }
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
```

The target of `alias` must be defined in the same file.

### AVR program memory

EmbCC has no address-space qualifiers. On AVR, `__flash` is predefined
as `__attribute__((__address_space__(1)))`, and that attribute is not
one EmbCC knows, so it is ignored with a warning:

```text
embcc: fl.c:1: warning: attribute '__address_space__' is not one EmbCC knows, and is ignored [-Wattributes]
```

The object is placed with ordinary data and copied to SRAM at startup.
`__flash` written after `const` (`const __flash char s[]`) or in a
parameter's type is a syntax error (`expected a type before
'__attribute__'`). `__memx` is not defined. `__attribute__((progmem))`
is ignored with the same warning. To keep data in flash and read it,
see [Data in program memory](embedded.md#data-in-program-memory).

The predefined macros `__BUILTIN_AVR_CLI`, `__BUILTIN_AVR_SEI`,
`__BUILTIN_AVR_NOP`, `__BUILTIN_AVR_SLEEP`, `__BUILTIN_AVR_SWAP` and
`__BUILTIN_AVR_WDR` are defined, but the `__builtin_avr_*` functions
are not implemented (`'__builtin_avr_sei' is not declared in 'f' — for a
call, add a prototype or define it first [E0001]`). Use inline
assembly, for example `__asm__ volatile("sei")`.

## Builtin functions

### General rules

A name is a builtin only in its `__builtin_` spelling (and, for the
atomics, its `__atomic_` or `__sync_` spelling). EmbCC treats no library
function name, such as `memcpy` or `printf`, as a builtin, with or without
[`-fno-builtin`](invoking.md#-fno-builtin).

A `__builtin_` name EmbCC does not implement is an undeclared function:

```text
embcc: f.c:1:29: error: '__builtin_fma' is not declared in 'f' — for a call, add a prototype or define it first [E0001]
```

Whether a builtin is available does not depend on the target, but its
code generation can: the tables below give the targets where it
compiles. Elsewhere it is refused when the function that uses it is
compiled.

### Integer bit operations

| Builtin | Result |
|---|---|
| `__builtin_clz(x)`, `clzl`, `clzll` | Number of leading zero bits in an `unsigned int`, `unsigned long`, `unsigned long long` |
| `__builtin_ctz(x)`, `ctzl`, `ctzll` | Number of trailing zero bits |
| `__builtin_popcount(x)`, `popcountl`, `popcountll` | Number of one bits |
| `__builtin_parity(x)`, `parityl`, `parityll` | Number of one bits, modulo 2 |
| `__builtin_ffs(x)`, `ffsl`, `ffsll` | One plus the index of the least significant one bit of an `int`, `long`, `long long`; 0 when `x` is 0 |
| `__builtin_clrsb(x)`, `clrsbl`, `clrsbll` | Number of leading bits equal to the sign bit, not counting the sign bit, of an `int`, `long`, `long long` |

All return `int`, take an integer argument, and compile on every target.
The operand width is the target's: `long` is 32 bits on Cortex-M, RV32
and AVR, and `int` is 16 bits on AVR. GCC leaves `clz` and `ctz` of zero
undefined; EmbCC returns a value, which code should not depend on.

### Byte swapping

| Builtin | Result |
|---|---|
| `__builtin_bswap16(x)` | `x` with its two bytes reversed, as `unsigned short` |
| `__builtin_bswap32(x)` | `x` with its four bytes reversed, as the 4-byte unsigned type: `unsigned int`, or `unsigned long` on AVR |
| `__builtin_bswap64(x)` | `x` with its eight bytes reversed, as the 8-byte unsigned type: `unsigned long` where `long` is 8 bytes, `unsigned long long` on Cortex-M, RV32 and AVR |

The result has the builtin's size on every target, and the argument is
converted to the result type before its bytes are reversed. All three
compile on every target. On Cortex-M, RV32 and AVR the swap is computed
with shifts, masks and ORs.

### Checked arithmetic

`__builtin_add_overflow(a, b, res)`, `__builtin_sub_overflow` and
`__builtin_mul_overflow` compute `a + b`, `a - b` or `a * b` as if with
unlimited range, store the result converted to the type `*res` points
to, and return 1 if the stored value differs from the mathematical
result and 0 otherwise. `a` and `b` are integers of any type; `res`
points to an integer. They compile on every target.

The result is `int`, not `_Bool` as in GCC. A wrong operand is refused:
`__builtin_add_overflow: operand 1 must be an integer, got double`, or
`__builtin_add_overflow: the third argument points at the integer to
store the result in`.

### Floating-point constants

| Builtin | Value | Type |
|---|---|---|
| `__builtin_inf()`, `__builtin_huge_val()` | Positive infinity | `double` |
| `__builtin_inff()`, `__builtin_huge_valf()` | Positive infinity | `float` |
| `__builtin_infl()`, `__builtin_huge_vall()` | Positive infinity | `long double` |
| `__builtin_nan("")`, `nanf`, `nanl` | A quiet NaN | `double`, `float`, `long double` |

These are constants and may appear in static initializers. The string
argument of `__builtin_nan` is not used: the result is always the same
NaN, whatever payload the string names. The `long double` forms compile
wherever `long double` values do; on RISC-V, operations on `long double`
are refused (`the RV32 backend cannot lower a 128-bit value yet ...`).

### Floating-point classification and sign

| Builtin | Result |
|---|---|
| `__builtin_fabs(x)`, `fabsf`, `fabsl` | `x` with its sign bit cleared |
| `__builtin_copysign(x, y)`, `copysignf`, `copysignl` | `x` with the sign of `y` |
| `__builtin_signbit(x)`, `signbitf`, `signbitl` | Nonzero if the sign bit of `x` is set, including for `-0.0` and a negative NaN |
| `__builtin_isnan(x)` | 1 if `x` is a NaN |
| `__builtin_isinf(x)` | 1 if `x` is an infinity |
| `__builtin_isinf_sign(x)` | 1 for positive infinity, -1 for negative infinity, 0 otherwise |
| `__builtin_isfinite(x)` | 1 if `x` is neither infinite nor NaN |
| `__builtin_isnormal(x)` | 1 if `x` is a normal number: not zero, subnormal, infinite or NaN |

`fabs` and `copysign` return the floating type their suffix names; the
others return `int`. The classification builtins take a floating
argument of any type, and refuse an integer (`__builtin_isnan takes a
floating-point argument`). All of them operate on the bits of the value:
they do not round, raise no floating-point exception, and call no
library.

They compile on every target. A 16-byte `long double` (x87's 80-bit
format on x86-64, IEEE binary128 on AArch64 ELF and Linux) is stored to
a stack slot and its sign and exponent are read and written there; the
other widths stay in registers. On RISC-V, where every operation on a
`long double` is refused, so are these.

### Square root

| Builtin | Result |
|---|---|
| `__builtin_sqrt(x)` | Square root, `double` |
| `__builtin_sqrtf(x)` | Square root, `float` |
| `__builtin_sqrtl(x)` | Square root, `long double` |

Where the target has a square-root instruction for the type, the builtin
is that instruction, and its result is correctly rounded:

| Target | Instruction for |
|---|---|
| x86-64 | `float`, `double` (`sqrtss`, `sqrtsd`) |
| AArch64 | `float`, `double` (`fsqrt`) |
| Cortex-M with the FPU enabled (`-eabihf` triples, or `-mfpu=` with `-mfloat-abi=softfp` or `hard`) | `float` (`vsqrt.f32`) |

Everywhere else -- a `double` on a Cortex-M FPU, soft-float Cortex-M,
RISC-V, AVR, and a 16-byte `long double` -- the builtin is a call to
`sqrt`, `sqrtf` or `sqrtl`, as with GCC, and the program must link a
math library that defines it. The precision is then that library's;
EmbCC's own `sqrtl` computes in `double` (see
[Libraries](libraries.md)). On RISC-V, `__builtin_sqrtl` is refused
with every other `long double` operation.

### Memory and string functions

`__builtin_memcpy`, `__builtin_memmove`, `__builtin_memset`,
`__builtin_memcmp`, `__builtin_memchr`, `__builtin_strlen`,
`__builtin_strcmp`, `__builtin_strncmp`, `__builtin_strcpy`,
`__builtin_strncpy`, `__builtin_strcat` and `__builtin_strchr` are the
library functions under another name: each becomes a call to the
function without the prefix, which the program must provide at link
time. `memcpy`, `memmove` and `memset` may also be expanded inline. See
[What the compiler emits calls to](embedded.md#what-the-compiler-emits-calls-to).

### Object size

`__builtin_object_size(ptr, type)` and
`__builtin_dynamic_object_size(ptr, type)` return the number of bytes
reachable through `ptr`, as a `size_t` constant. EmbCC answers with the
object's size when `ptr` is the address of a named object that is not an
array or a pointer (`&s`). For every other argument it returns the value
that means "unknown": `(size_t)-1` for `type` 0 and 1, and 0 for `type` 2
and 3. A `_FORTIFY_SOURCE` header reads that value as "do not check".

### Compile-time builtins

| Builtin | Result |
|---|---|
| `__builtin_constant_p(expr)` | 1 if `expr` is an integer constant expression, else 0. It is decided when the expression is checked, before optimization: a `const` variable, a floating constant, and a parameter that is constant after inlining all give 0. `expr` is not evaluated |
| `__builtin_types_compatible_p(type1, type2)` | 1 if the two types are the same after top-level qualifiers are removed, else 0. An integer constant expression. An array of unknown size and one of known size give 0 |
| `__builtin_choose_expr(const, expr1, expr2)` | `expr1` if `const` is nonzero, else `expr2`. The arm not chosen is not type-checked. A condition that is not constant is refused: `__builtin_choose_expr needs a constant condition` |
| `__builtin_offsetof(type, designator)` | The byte offset of a member, as an integer constant expression. The designator may use `.member` and `[constant]`. This is what `<stddef.h>`'s `offsetof` expands to |
| `__builtin_LINE()` | The line of the call, as an `int` constant |
| `__builtin_FILE()` | The name of the source file, as a string literal |
| `__builtin_expect(expr, value)`, `__builtin_expect_with_probability(expr, value, p)` | `expr`; the hint is discarded |
| `__builtin_assume_aligned(ptr, align, ...)` | `ptr`; the hint is discarded |

`__builtin_FUNCTION()` is not implemented; use `__func__`.

### Control flow

| Builtin | Effect |
|---|---|
| `__builtin_unreachable()` | Emits the target's trap instruction. Reaching it stops the program; it is not undefined behavior |
| `__builtin_trap()` | Emits the target's trap instruction |
| `__builtin_prefetch(addr, ...)` | Evaluates its arguments and emits nothing |

The trap is `ud2` on x86-64, `udf` on AArch64 and Cortex-M, and `unimp`
on RISC-V. AVR has no trapping instruction, and the trap is a jump to
itself. A call to either builtin ends a path for the missing-return
check, as a call to a `noreturn` function does.

### Stack and frames

| Builtin | Result | Targets |
|---|---|---|
| `__builtin_alloca(size)` | A pointer to `size` bytes in the current function's frame, freed when the function returns | All but AVR |
| `__builtin_alloca_with_align(size, align)` | As `__builtin_alloca`, aligned to `align` bits, which must be a constant power of two of at least 8 | All but AVR |
| `__builtin_frame_address(level)` | The frame address of the current function (`level` 0) or of a caller, found by following the saved frame pointers | x86-64, AArch64 |
| `__builtin_return_address(level)` | The return address of the current function (`level` 0) or of a caller | x86-64, AArch64 |

`level` must be a non-negative integer constant (`__builtin_frame_address
needs a non-negative constant level`). On AVR, `alloca` is refused as a
variable-length array is (`the AVR backend cannot lower a variable-length
array yet (function f)`). On Cortex-M, RISC-V and AVR the frame builtins
are refused:

```text
embcc: r.c:1: error: the ARMv7-M backend cannot lower this operation at 64 bits yet (function f) [frameaddr w=8 size=4]
```

A function that calls `alloca` is never inlined.

### Variable arguments

`__builtin_va_list` is the type `char *`, the representation of
`va_list` on every target. `__builtin_va_start(ap, last)`,
`__builtin_va_arg(ap, type)`, `__builtin_va_copy(dest, src)` and
`__builtin_va_end(ap)` are what `<stdarg.h>`'s macros expand to. Using
`va_start` in a function that is not variadic is refused (`va_start in
'f', which is not variadic`). Passing and retrieving structures through
`...` is described in [C language](c-language.md).

### Builtins that are not supported

These are refused as undeclared functions, as shown in [General
rules](#general-rules): `__builtin_abs`, `__builtin_assume`,
`__builtin_bitreverse32` and the other widths,
`__builtin_rotateleft32` and the other rotates, `__builtin_complex`,
`__builtin_fma`, `__builtin_fpclassify`, `__builtin_isgreater` and the
other comparison macros' builtins, `__builtin_setjmp`,
`__builtin___clear_cache`, `__builtin_FUNCTION`,
`__builtin_speculation_safe_value`, `__builtin_shufflevector`, the
target-specific families (`__builtin_ia32_*`, `__builtin_avr_*`), and
Clang's `__c11_atomic_*`. `__builtin_addressof`, `__builtin_bit_cast`
and `__builtin_is_constant_evaluated` exist in C++ only; see
[C++](cxx.md).

## Atomic builtins

### The builtins

EmbCC implements GCC's two families.

| Builtin | Operation |
|---|---|
| `__atomic_load_n(p, order)` | Returns `*p` |
| `__atomic_store_n(p, v, order)` | `*p = v` |
| `__atomic_exchange_n(p, v, order)` | `*p = v`, returns the old value |
| `__atomic_compare_exchange_n(p, expected, desired, weak, success, failure)` | If `*p == *expected`, stores `desired` and returns 1; otherwise copies `*p` into `*expected` and returns 0 |
| `__atomic_load(p, ret, order)`, `__atomic_store(p, val, order)`, `__atomic_exchange(p, val, ret, order)`, `__atomic_compare_exchange(p, expected, desired, weak, success, failure)` | The generic forms: the values are passed through pointers, so the object may be of any type of 1, 2, 4, 8 or 16 bytes, such as a `double` or a small structure |
| `__atomic_fetch_OP(p, v, order)` | `*p = *p OP v`, returns the old value. `OP` is `add`, `sub`, `and`, `or`, `xor` or `nand` |
| `__atomic_OP_fetch(p, v, order)` | As above, returns the new value |
| `__atomic_test_and_set(p, order)` | Sets the byte at `p`, returns whether it was set |
| `__atomic_clear(p, order)` | Clears the byte at `p` |
| `__atomic_thread_fence(order)`, `__atomic_signal_fence(order)` | A full memory barrier |
| `__atomic_always_lock_free(size, p)`, `__atomic_is_lock_free(size, p)` | See below |
| `__sync_fetch_and_OP(p, v, ...)`, `__sync_OP_and_fetch(p, v, ...)` | As the `__atomic` forms, sequentially consistent |
| `__sync_val_compare_and_swap(p, old, new, ...)` | Returns the old value |
| `__sync_bool_compare_and_swap(p, old, new, ...)` | Returns 1 if the swap happened |
| `__sync_lock_test_and_set(p, v, ...)` | Stores `v`, returns the old value |
| `__sync_lock_release(p, ...)` | Stores 0 |
| `__sync_synchronize()` | A full memory barrier |

The `_n` and arithmetic forms take an integer or a pointer of 1, 2, 4,
8 or 16 bytes. A wrong argument count or type is refused, for example
`__atomic_load_n takes 2 arguments, not 3` or `__atomic_fetch_add works
on an integer or pointer of 1, 2, 4, 8 or 16 bytes, not double`. The
`__sync` builtins accept, and ignore, the trailing list of variables
GCC allows.

Every operation is sequentially consistent. The memory-order arguments
are accepted and do not change the code: `__ATOMIC_RELAXED` produces the
same instructions as `__ATOMIC_SEQ_CST`. `__atomic_signal_fence` emits
the same barrier as `__atomic_thread_fence`. No operation calls a
library: each is inline or refused.

`__atomic_always_lock_free` and `__atomic_is_lock_free` are integer
constant expressions, usable in `_Static_assert`. The size must be a
constant (`__atomic_is_lock_free needs a constant size`). They answer 1
for sizes 1, 2, 4 and 8 and 0 otherwise, on every target; the answer
does not reflect the table below.

### Sizes per target

| Operation | x86-64 | AArch64 | Cortex-M | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|
| Load, store | 1, 2, 4, 8, 16 | 1, 2, 4, 8, 16 | 1, 2, 4 | 1, 2, 4 | 1, 2, 4, 8 | 1 |
| Exchange, fetch-and-op, compare-exchange, test-and-set | 1, 2, 4, 8, 16 | 1, 2, 4, 8, 16 | 1, 2, 4 | 4 | 4, 8 | None |
| Fences | Yes | Yes | Yes | Yes | Yes | Yes (no instruction) |

16-byte operations need `__int128` or a 16-byte object through the
generic forms. Other sizes are refused, for example:

```text
embcc: a.c:1: error: the RV32 backend cannot lower an atomic narrower than four bytes (the A extension has no such form, and a read-modify-write of the containing word is not atomic against its neighbours) yet (function f) [xadd w=4 size=1]
embcc: a.c:1: error: the ARMv7-M backend cannot lower this operation at 64 bits yet (function f) [xadd w=8 size=8]
embcc: a.c:1: error: an atomic access of 8 bytes is not one access on this target (it moves 4 at once): the halves could be split by an interrupt or another core
embcc: a.c:1: error: the AVR backend cannot lower xadd yet (function f) [xadd w=4 size=2]
```

The instructions used on each embedded target are described in
[Embedded programming](embedded.md). `_Atomic` objects and the
`<stdatomic.h>` interface are described in [C language](c-language.md).

## Vector extensions

EmbCC has no vector types. `__attribute__((vector_size(N)))` is refused:

```text
embcc: v.c:1: error: __attribute__((vector_size)) is not supported: the type would stay a scalar: EmbCC's vector IR comes from the auto-vectorizer and only x86-64 lowers it, so a vector TYPE has no representation in the front end or on three of four targets
```

Clang's `ext_vector_type` is not recognized; it draws the unknown
attribute warning, and the type stays a scalar. `__builtin_shufflevector`
is not implemented. The optimizer's automatic vectorization is described
in [Optimization](optimization.md#-fvectorize--fno-vectorize).

## Feature-test operators

### `__has_include` and `__has_include_next`

`__has_include(<header>)` and `__has_include("header")` are 1 if the
header can be found by the search an `#include` of the same name would
make, and 0 otherwise. `__has_include_next` asks the same about the
search `#include_next` would make.

### `__has_builtin`

`__has_builtin(name)` is 1 for:

- the integer bit operations, byte swaps, checked arithmetic,
  floating-point constants, classification and square-root builtins,
  `__builtin_alloca`, `__builtin_alloca_with_align`,
  `__builtin_assume_aligned`, `__builtin_constant_p`,
  `__builtin_expect`, `__builtin_expect_with_probability`,
  `__builtin_frame_address`, `__builtin_return_address`, the memory and
  string builtins, `__builtin_object_size`,
  `__builtin_dynamic_object_size`, `__builtin_offsetof`,
  `__builtin_prefetch`, `__builtin_trap`, `__builtin_unreachable`,
  `__builtin_va_arg`, `__builtin_va_copy`, `__builtin_va_end` and
  `__builtin_va_start`;
- every `__atomic_` and `__sync_` builtin listed in [Atomic
  builtins](#atomic-builtins).

It is 0 for `__builtin_types_compatible_p`, `__builtin_choose_expr`,
`__builtin_LINE`, `__builtin_FILE` and `__builtin_va_list`, although
EmbCC implements them, and for every name it does not implement. The
answer does not depend on the target: a builtin that the target's code
generator refuses still answers 1.

### `__has_attribute`

`__has_attribute(name)` accepts `name`, `__name__` and a `gnu::` or
`__gnu__::` scope. It is 1 for: `access`, `aligned`, `alloc_align`,
`alloc_size`, `always_inline`, `artificial`, `cold`, `const`,
`deprecated`, `fallthrough`, `flatten`, `format`, `gnu_inline`, `hot`,
`leaf`, `malloc`, `may_alias`, `maybe_unused`, `no_sanitize`,
`no_sanitize_address`, `nodiscard`, `noinline`, `noipa`, `nonnull`,
`noreturn`, `nothrow`, `packed`, `pure`, `returns_nonnull`, `section`,
`sentinel`, `unused`, `used`, `visibility`, `warn_unused_result` and
`weak`. It is also 1 for `abi_tag`, `externally_visible`, `format_arg`,
`likely`, `unavailable` and `unlikely`, which C does not recognize (each
draws the unknown attribute warning).

It is 0 for every other name, including attributes EmbCC implements:
`alias`, `constructor`, `destructor`, `embcc_sret`, `interrupt`, `pcs`
and `signal`. Test for those with `__EMBCC__` and the target macros.

### `__has_c_attribute` and `__has_cpp_attribute`

`__has_c_attribute(name)` is defined in C and is 0 for every name,
although EmbCC accepts the C23 attributes `[[noreturn]]`,
`[[deprecated]]`, `[[nodiscard]]`, `[[maybe_unused]]` and
`[[fallthrough]]`. `__has_cpp_attribute` is defined only in C++.

### `__has_feature` and `__has_extension`

Both answer the same. They are 1 for `c_static_assert`,
`c_generic_selections`, `c_atomic`, `c_alignas`, `c_alignof`,
`c_thread_local`, `tls`, `attribute_deprecated_with_message`,
`attribute_unavailable_with_message` and `enumerator_attributes`, and 0
for every other name. Three of these answers overstate what C accepts:
`tls` is 1 on Mach-O and COFF targets, where `__thread` is refused;
`unavailable` is not recognized; and an attribute on an enumerator is a
syntax error.

### Using the operators

Each operator counts as defined, so the guarded form used by portable
headers works:

```c
#if defined(__has_include) && __has_include(<stdatomic.h>)
#include <stdatomic.h>
#endif
```

`#ifdef` and `#elifdef` see them the same way. In C, an operator is
evaluated only where it is written in the `#if` line itself. One that a
macro expands to is not:

```c
#define HAS(x) __has_builtin(x)
#if HAS(__builtin_clz)        /* error: trailing junk in #if expression */
#endif
```

In C++ both forms work.

## Preprocessor extensions

### Supported

| Feature | Notes |
|---|---|
| `#include_next` | Continues the search after the directory the current file was found in. A missing file is `cannot find a NEXT include file "h.h"` |
| `#warning text` | Prints `warning: #warning: text` and continues |
| `#elifdef`, `#elifndef` | C23 |
| `#embed "file"` | C23; expands to the file's bytes as a comma-separated list of integers |
| `__VA_OPT__(tokens)` | C23; the tokens appear only when the variadic arguments are not empty |
| `_Pragma("...")` | The operator form of `#pragma`, usable in a macro |
| GNU line markers, `# 12 "file.c"` | Read as `#line` |
| Named variadic macro parameters, `#define F(args...)` | GNU; `args` names the variable arguments |
| GNU comma elision, `, ## __VA_ARGS__` | With no variable arguments the comma is removed, so `LOG("x")` with `#define LOG(f, ...) printf(f, ## __VA_ARGS__)` is `printf("x")` |
| `__COUNTER__` | GNU; 0, 1, 2, ... at each use in the translation unit |
| `__FILE_NAME__`, `__BASE_FILE__`, `__INCLUDE_LEVEL__` | GNU; the current file's last path component, the main file's name, and the current `#include` depth (0 in the main file) |
| `#include MACRO` | The operand is macro-replaced, and must then be `"..."` or `<...>` |
| `defined` on the feature-test operators | See [Using the operators](#using-the-operators) |

### Not supported

| Feature | Behavior |
|---|---|
| `#ident`, `#sccs`, `#assert`, `#unassert`, `#import` | `unknown directive '#ident'` |
| `__TIMESTAMP__` | Not defined; the name is left as written |
| `$` in a macro name | Not accepted as part of an identifier |

## Pragmas

### `#pragma pack`

`#pragma pack` sets the maximum alignment of the members of the
structures and unions defined after it. It takes these forms:

| Form | Effect |
|---|---|
| `#pragma pack(N)` | Members are aligned to at most `N` bytes. `N` is 1, 2, 4, 8 or 16; 0 removes the limit |
| `#pragma pack()` | Removes the limit |
| `#pragma pack(push)` | Saves the current limit |
| `#pragma pack(push, N)` | Saves the current limit and sets `N` |
| `#pragma pack(pop)` | Restores the last saved limit |
| `#pragma pack(show)` | Accepted; does nothing |

The limit applies to a structure as a whole, at the point where its
definition starts, including a definition inside a function. `_Pragma("pack(1)")`
is the same as `#pragma pack(1)`. The argument is not macro-expanded, as
in GCC on ELF targets.

| Refused | Diagnostic |
|---|---|
| A value other than 0, 1, 2, 4, 8 or 16, or a macro name | `#pragma pack wants 1, 2, 4, 8 or 16` |
| A named push or pop, `pack(push, name, N)` | `#pragma pack(push, name) is not supported` |
| `pack(pop, ...)` with arguments | `#pragma pack(pop, ...) with a name or count is not supported` |
| `pack(pop)` with nothing saved | `#pragma pack(pop) without a matching push` |
| More than 32 nested pushes | `#pragma pack(push) nested too deeply` |
| Inside a structure body | `#pragma pack inside a struct body is not supported: put it before the struct` |
| Without parentheses, `#pragma pack 1` | `#pragma pack needs its arguments in parentheses` |
| In C++ | `#pragma pack is not supported in C++: it would change the layout and EmbCC would ignore it. Use __attribute__((packed)) on the struct` |

### `#pragma once`

A file containing `#pragma once` is not read again by a later
`#include`. The same file is recognized by its path, or, when it is
reached by another spelling of the path, by its contents, as GCC does.

### `#pragma push_macro` and `#pragma pop_macro`

`#pragma push_macro("NAME")` saves the current definition of the macro
`NAME`, or the fact that it has none. `#pragma pop_macro("NAME")`
restores the most recently saved one: `NAME` is redefined as it was, or
undefined if it was undefined then. Saves nest. A `pop_macro` with no
matching `push_macro` does nothing, as in GCC. Both work as `_Pragma`
operators, `_Pragma("push_macro(\"NAME\")")`.

### `#pragma weak`

`#pragma weak NAME` makes `NAME` a weak symbol, as
`__attribute__((weak))` on its declaration does: a weak definition, or
a weak reference that resolves to 0 when nothing defines it. The pragma
may come before or after the declaration. `#pragma weak NAME = TARGET`
also makes the function `NAME` an alias of `TARGET`, which must be
defined in the same file. Refused forms:

| Refused | Diagnostic |
|---|---|
| An alias for a variable | `#pragma weak v = t: an alias is supported for a function, and 'v' is a variable` |
| An alias for an undeclared name | `#pragma weak f = t: 'f' is not declared in this file, and the alias needs its type` |
| Inside a structure body | `#pragma weak inside a struct body is not supported: put it before the struct` |
| In C++ | `#pragma weak is not supported in C++: use __attribute__((weak)) on the declaration` |

### `#pragma GCC diagnostic`

`#pragma GCC diagnostic KIND "-WNAME"` sets the warning `NAME` from the
pragma's position on; `#pragma clang diagnostic` and the `_Pragma`
operator form are the same. Each warning is decided by the pragmas
before it in the preprocessed text, so a pragma in a header applies
after the `#include` unless the header pops it.

| Form | Effect |
|---|---|
| `#pragma GCC diagnostic ignored "-WNAME"` | `NAME` is not reported |
| `#pragma GCC diagnostic warning "-WNAME"` | `NAME` is reported as a warning, even when the command line does not enable it, and even under `-Werror` |
| `#pragma GCC diagnostic error "-WNAME"` | `NAME` is reported as an error |
| `#pragma GCC diagnostic push` | Saves the state of every warning |
| `#pragma GCC diagnostic pop` | Restores the state last saved; with nothing saved, returns every warning to the command line's setting |

A pragma that names a warning EmbCC does not have is ignored without a
diagnostic; EmbCC never reports that warning, so the pragma would change
nothing. Other kinds, such as `ignored_attributes`, are ignored. `-w`
and the suppression of warnings in system headers apply whatever the
pragmas say. A warning located in a header that is included more than
once is decided at the header's last inclusion.

### Every other pragma is ignored

Every other `#pragma` and `_Pragma` is removed without a diagnostic, and
does not appear in `-E` output. This includes pragmas that change a
program's meaning or its diagnostics in GCC and Clang:

| Pragma | Use instead |
|---|---|
| `#pragma GCC visibility push(...)`, `pop` | `__attribute__((visibility(...)))` on each declaration |
| `#pragma GCC poison`, `#pragma GCC system_header`, `#pragma message`, `#pragma GCC optimize`, `#pragma redefine_extname` | None |
| `#pragma STDC FP_CONTRACT`, `FENV_ACCESS`, `CX_LIMITED_RANGE` | None |

## Accepted without effect

These constructs are accepted, have no effect, and draw no diagnostic.
Each is described in its section above.

- Every pragma except `pack`, `once`, `push_macro`, `pop_macro`, `weak`
  and `GCC diagnostic`.
- `packed` and `section` on a single structure member.
- `pcs` on a function-pointer parameter.
- The memory-order arguments of the atomic builtins.
- The payload string of `__builtin_nan`.
- `weak` and `visibility` on COFF output, and `visibility` on Mach-O
  output.
